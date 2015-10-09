#include <assert.h>
#include "evt_tls.h"



// 0 - client
// 1 - server
// XXX: make this enum
int evt_tls_get_role(const evt_tls_t *t)
{
    assert(t != NULL);
    return t->ssl->server;
}

void evt_tls_set_role(evt_tls_t *t, int role)
{
    assert(t != NULL && (role  == 0 || role == 1));
    if ( 1 == role ) {
        SSL_set_accept_state(t->ssl);
    }
    else {
        SSL_set_connect_state(t->ssl);
    }
}

SSL_CTX *evt_get_SSL_CTX(const evt_ctx_t *ctx)
{
    return ctx->ctx;
}

SSL *evt_get_ssl(const evt_tls_t *tls)
{
    return tls->ssl;
}


static void tls_begin(void)
{
    SSL_library_init();
    SSL_load_error_strings();
    ERR_load_BIO_strings();
    OpenSSL_add_all_algorithms();
    ERR_load_crypto_strings();
}


evt_tls_t *evt_ctx_get_tls(evt_ctx_t *d_eng)
{
    evt_tls_t *con = malloc(sizeof(evt_tls_t));
    if ( !con ) {
        return NULL;
    }
    memset( con, 0, sizeof *con);

    SSL *ssl  = SSL_new(d_eng->ctx);

    if ( !ssl ) {
        free(con);
        return NULL;
    }
    con->ssl = ssl;

    //use default buf size for now.
    BIO_new_bio_pair(&(con->ssl_bio_), 0, &(con->app_bio_), 0);

    SSL_set_bio(con->ssl, con->ssl_bio_, con->ssl_bio_);

    QUEUE_INIT(&(con->q));
    QUEUE_INSERT_TAIL(&(d_eng->live_con), &(con->q));

    con->writer = d_eng->writer;
    con->evt_ctx = d_eng;

    return con;
}

void evt_ctx_set_writer(evt_ctx_t *ctx, net_wrtr my_writer)
{
    assert( ctx->writer == NULL);
    ctx->writer = my_writer;
    assert( ctx->writer != NULL);
}

int evt_ctx_set_crt_key(evt_ctx_t *tls, char *crtf, char *key)
{
    //SSL_CTX_set_verify(tls->ctx, SSL_VERIFY_NONE, uv__tls_verify_peer);
    SSL_CTX_set_verify(tls->ctx, SSL_VERIFY_NONE, NULL);

    int r = SSL_CTX_use_certificate_file(tls->ctx, crtf, SSL_FILETYPE_PEM);
    if(r != 1) {
        return -1;
    }
    tls->cert_set = 1;

    r = SSL_CTX_use_PrivateKey_file(tls->ctx, key, SSL_FILETYPE_PEM);
    if(r != 1) {
        return -1;
    }

    r = SSL_CTX_check_private_key(tls->ctx);
    if(r != 1) {
        return -1;
    }
    tls->key_set = 1;
    return 0;
}

int evt_ctx_init(evt_ctx_t *tls)
{
    tls_begin();

    //Currently we support only TLS, No DTLS
    tls->ctx = SSL_CTX_new(SSLv23_method());
    if ( !tls->ctx ) {
        return ENOMEM;
    }

    long options = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3;
    SSL_CTX_set_options(tls->ctx, options);

    SSL_CTX_set_mode(tls->ctx, SSL_MODE_AUTO_RETRY |
        SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER        |
        SSL_MODE_ENABLE_PARTIAL_WRITE              |
        SSL_MODE_RELEASE_BUFFERS
    );

    tls->cert_set = 0;
    tls->key_set = 0;
    tls->ssl_err_ = 0;
    tls->writer = NULL;

    QUEUE_INIT(&(tls->live_con));
    return 0;
}

int evt_ctx_is_crtf_set(evt_ctx_t *t)
{
    return t->cert_set && t->key_set;
}

int evt_ctx_is_key_set(evt_ctx_t *t)
{
    return t->key_set;
}


static int evt__send_pending(evt_tls_t *c, void *buf)
{
    assert( c != NULL);
    int pending = BIO_pending(c->app_bio_);
    if ( !(pending > 0) )
        return 0;

    int p = BIO_read(c->app_bio_, buf, pending);
    assert(p == pending);

    assert( c->writer != NULL && "You need to set network writer first");
    c->writer(c, buf, p);
    return p;
}

static int evt__tls__op(evt_tls_t *c, enum tls_op_type op, void *buf, int sz)
{
    int r = 0;
    int bytes = 0;
    char tbuf[16*1024] = {0};

    switch ( op ) {
        case EVT_TLS_OP_HANDSHAKE: {
            r = SSL_do_handshake(c->ssl);
            bytes = evt__send_pending(c, tbuf);
            if  (1 == r) {
                if (!evt_tls_get_role(c)) { //client
                    assert(c->connect_cb != NULL );
                    c->connect_cb(c, r);
                }
                else { //server
                    assert(c->accept_cb != NULL );
                    c->accept_cb(c, r);
                }
            }
            break;
        }

        case EVT_TLS_OP_READ: {
            r = SSL_read(c->ssl, tbuf, sizeof(tbuf));
            bytes = evt__send_pending(c, tbuf);
            if ( r > 0 ) { // XXX handle r == 0, which is shutdown
                assert(c->read_cb != NULL);
                c->read_cb(c, tbuf, r);
            }
            break;
        }

        case EVT_TLS_OP_WRITE: {
            r = SSL_write(c->ssl, buf, sz);
            bytes = evt__send_pending(c, tbuf);
            if ( r > 0  &&  c->write_cb) {
                    c->write_cb(c, r);
            }
            break;
        }

        case EVT_TLS_OP_SHUTDOWN: {
            r = SSL_shutdown(c->ssl);
            bytes = evt__send_pending(c, tbuf);
            if ( (1 == r)  && c->close_cb) {
                c->close_cb(c, r);
            }
            break;
        }

        default:
            assert( 0 && "Unsupported operation");
            break;
    }
    return r;
}

int evt_tls_feed_data(evt_tls_t *c, void *data, int sz)
{
    int rv =  BIO_write(c->app_bio_, data, sz);
    assert( rv == sz);

    //if handshake is not complete, do it again
    if (SSL_is_init_finished(c->ssl)) {
        rv = evt__tls__op(c, EVT_TLS_OP_READ, NULL, 0);
    }
    else {
        rv = evt__tls__op(c, EVT_TLS_OP_HANDSHAKE, NULL, 0);
    }
    return rv;
}


int evt_tls_connect(evt_tls_t *con, evt_conn_cb on_connect)
{
    con->connect_cb = on_connect;
    SSL_set_connect_state(con->ssl);
    return evt__tls__op(con, EVT_TLS_OP_HANDSHAKE, NULL, 0);
}

int evt_tls_accept( evt_tls_t *svc, evt_accept_cb cb)
{
    assert(svc != NULL);
    SSL_set_accept_state(svc->ssl);
    svc->accept_cb = cb;
    return 0;
}

int evt_tls_write(evt_tls_t *c, void *msg, int str_len, evt_write_cb on_write)
{
    c->write_cb = on_write;
    return evt__tls__op(c, EVT_TLS_OP_WRITE, msg, str_len);
}

// read only register the callback to be made
int evt_tls_read(evt_tls_t *c, evt_read_cb on_read)
{
    assert(c != NULL);
    c->read_cb = on_read;
    return 0;
}

int evt_tls_close(evt_tls_t *tls, evt_close_cb cb)
{    
    assert(tls != NULL);
    tls->close_cb = cb;
    return evt__tls__op(tls, EVT_TLS_OP_SHUTDOWN, NULL, 0);
}

//need impl
int evt_tls_force_close(evt_tls_t *tls, evt_close_cb cb);



int evt_tls_delete(evt_tls_t *tls)
{
    BIO_free(tls->app_bio_);
    tls->app_bio_ = NULL;

    SSL_free(tls->ssl);
    tls->ssl = NULL;

    QUEUE_REMOVE( &(tls->q));
    QUEUE_INIT( &(tls->q) );

    free(tls);
    tls = NULL;
    return 0;
}

//clean up calls
void evt_ctx_free(evt_ctx_t *ctx) {
    QUEUE* qh;
    evt_tls_t *tls = NULL;

    //clean all pending connections
    QUEUE_FOREACH(qh, &ctx->live_con) {
        tls = QUEUE_DATA(qh, evt_tls_t, q);
        //force close all connections and clean
        //XXX add code here for cleaning, wait for QUEUE_MOVE in libuv
        evt__tls__op(tls, EVT_TLS_OP_SHUTDOWN, NULL, 0);
    }

    SSL_CTX_free(ctx->ctx);
    ctx->ctx = NULL;

    ERR_remove_state(0);
    ENGINE_cleanup();
    CONF_modules_unload(1);
    ERR_free_strings();
    EVP_cleanup();
    sk_SSL_COMP_free(SSL_COMP_get_compression_methods());
    //SSL_COMP_free_compression_methods();
    CRYPTO_cleanup_all_ex_data();
}
