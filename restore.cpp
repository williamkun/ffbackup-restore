#include "restore.h"
#include "commonfunctions.h"
#include "ffbuffer.h"

#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>

#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <librsync.h>


using namespace std;

#define SSL_DFLT_HOST  "localhost"
#define SSL_DFLT_PORT  "16903"

extern const char *CFG_PATH;

const char version = 0x02;
const int MAX_BUFFER_SIZE = 1024;


extern char *optarg;
static BIO  *bio_err = 0;
static int  verbose = 0;

static int  err_exit( const char * );
static int  ssl_err_exit( const char * );
static void sigpipe_handle( int );
static int  ip_connect(int type, int protocol, const char *host, const char *serv);
static void check_certificate( SSL *, int );

static int password_cb( char *buf, int num, int rwflag, void *userdata )
{
    char password[] = "client";
    int len = strlen( password );

    if ( num < len + 1 )
        len = 0;
    else
        strcpy( buf, password );

    return( len );
}

static int err_exit( const char *string )
{
    fprintf( stderr, "%s\n", string );
    exit(0);
}

static int ssl_err_exit( const char *string )
{
    BIO_printf( bio_err, "%s\n", string );
    ERR_print_errors( bio_err );
    exit(0);
}

static void sigpipe_handle( int x )
{
}


/**
 * create a socket
 * and connect to host:serv (TCP)
 * or set the default destination host:serv (UDP)
 *
 * type: SOCK_STREAM or SOCK_DGRAM
 * protocol: IPPROTO_TCP or IPPROTO_UDP
 * host: host name of remote host
 * serv: service name
 *
 * On success, a file descriptor for the new socket is returned
 * On error, -1 is returned
 */
static int ip_connect(int type, int protocol, const char *host, const char *serv)
{
    struct addrinfo hints, *res, *saved;
    int n, sockfd;

    bzero(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = type;
    hints.ai_protocol = protocol;
    n = getaddrinfo(host, serv, &hints, &res);
    if(n != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(n));
        return -1;
    }
    saved = res;
    while(res)
    {
        sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if(sockfd >= 0)
        {
            if(connect(sockfd, res->ai_addr, res->ai_addrlen) == 0)
                break;
        }
        res = res->ai_next;
    }
    if(res == NULL)
    {
        perror("ip_connect");
        sockfd = -1;
    }
    freeaddrinfo(saved);
    return sockfd;
}


static void check_certificate( SSL *ssl, int required )
{
    X509 *peer;

    /* Verify server certificate */
    if ( SSL_get_verify_result( ssl ) != X509_V_OK )
        ssl_err_exit( "Certificate doesn't verify" );

    /* Check the common name */
    peer = SSL_get_peer_certificate( ssl );

    if ( ! peer  &&  required )
        err_exit( "No peer certificate" );
}


/**
 * the error's output
 * msg: the errno message
 */
void die(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}




void restore_get_prj(SSL *ssl, vector<string> &prj_list)
{
    char buffer[2];
    char command = 0x08;
    uint32_t file_count = 0;
    uint32_t i = 0;
    char *prj_name;

    buffer[0] = version;
    buffer[1] = command;

    ssl_write_wrapper(ssl, buffer, 2); 
    ssl_read_wrapper(ssl, buffer, 2); 
    ssl_read_wrapper(ssl, &file_count, 4);
    file_count = ntoh32(file_count);
    fwrite(&file_count, 1, sizeof(file_count), stdout);
    for(i = 0; i < file_count; i++)
    {
        prj_name = read_string(ssl);
        fwrite(prj_name, 1, strlen(prj_name) + 1, stdout);
        prj_list.push_back(prj_name);
        free(prj_name);
    }
}

void restore_get_time_line(SSL *ssl, const char *prj_name)
{
    char buffer[2];
    char command = 0x09;
    uint32_t backup_id;
    uint32_t backup_time;
    uint32_t list_size;
    uint32_t i = 0;

    buffer[0] = version;
    buffer[1] = command;
    ssl_write_wrapper(ssl, buffer, 2);
    ssl_write_wrapper(ssl, prj_name, strlen(prj_name) + 1);
    ssl_read_wrapper(ssl, buffer, 2); 
    ssl_read_wrapper(ssl, &list_size, 4);
    list_size = ntoh32(list_size);
    fwrite(&list_size, 1, sizeof(list_size), stdout);
    for(i = 0; i < list_size; i++)
    {
        ssl_read_wrapper(ssl, &backup_id, 4);
        ssl_read_wrapper(ssl, &backup_time, 4);
        backup_id = ntoh32(backup_id);
        backup_time = ntoh32(backup_time);
        fwrite(&backup_id, 1, sizeof(backup_id), stdout);
        fwrite(&backup_time, 1, sizeof(backup_time), stdout);
    }
}

void restore(SSL *ssl, const char *prj_name, uint32_t backup_id, const char *prj_restore_dir)
{
    char buffer[2];
    char command = 0x0A;
    uint32_t list_size;
    uint32_t i = 0;
    uint64_t file_size = 0;
    uint64_t total_read = 0;
    char *file_path;
    char file_type;
    FILE *file;
    char file_buffer[MAX_BUFFER_SIZE];
    char backup_id_str[8];
    string dir_name;
    dir_name.append(prj_name);
    sprintf(backup_id_str,"%d",backup_id);
    dir_name.append(backup_id_str);
    //1.change to the project path

    if(chdir(prj_restore_dir) == -1)
    {
        fputs("Chdir error.\n",stderr);
        exit(1);
    }
    if(mkdir(dir_name.c_str(),S_IRWXU|S_IRWXG|S_IROTH|S_IXOTH) == -1)
    {
        fputs("Mkdir error.\n",stderr);
        exit(1);
    }
    if(chdir(dir_name.c_str()) == -1)
    {
        fputs("Chdir error.\n",stderr);
        exit(1);
    }
    buffer[0] = version;
    buffer[1] = command;
    ssl_write_wrapper(ssl, buffer, 2);
    ssl_write_wrapper(ssl, prj_name, strlen(prj_name) + 1);
    backup_id = hton32(backup_id);
    ssl_write_wrapper(ssl, &backup_id, 4);
    
    ssl_read_wrapper(ssl, buffer, 2);
    ssl_read_wrapper(ssl, &list_size, 4);
    list_size = ntoh32(list_size);
    for(i = 0; i < list_size; i++)
    {
        file_path = read_string(ssl);
        ssl_read_wrapper(ssl, &file_type, 1);
        if(file_type == 'd')
        {
            if(mkdir(file_path,S_IRWXU|S_IRWXG|S_IROTH|S_IXOTH) == -1)
            {
                fputs("Mkdir error.\n",stderr);
                exit(1);
            }
        }
        else
        {
            file = fopen(file_path, "wb");
            if(!file)
            {
                fputs("Fopen error.\n",stderr);
                exit(1);
            }
            ssl_read_wrapper(ssl, &file_size, 8);
            file_size = ntoh64(file_size);
            while((total_read + MAX_BUFFER_SIZE) < file_size)
            {
                ssl_read_wrapper(ssl, file_buffer, MAX_BUFFER_SIZE);
                fwrite(file_buffer, 1, MAX_BUFFER_SIZE, file);
                total_read += MAX_BUFFER_SIZE;
            }
            if(total_read != file_size)
            {
                ssl_read_wrapper(ssl, file_buffer, file_size - total_read);
                fwrite(file_buffer, 1, file_size - total_read, file);
            }
            fclose(file);
            total_read = 0;
        }
        fprintf(stderr, "%u", i);
        free(file_path);
    }
}

int main(int argc, char **argv)
{


    int c,sock;
    SSL_CTX *ctx;
    const SSL_METHOD *meth;
    SSL *ssl;
    BIO *sbio;
    char *cafile = NULL;
    char *cadir = NULL;
    char *certfile = NULL;
    char *keyfile = NULL;
    const char *host = NULL;
    const char *port = NULL;
    const char *instruction = NULL;
    const char *prj_name = NULL;
    const char *order = NULL;
    const char *dir = NULL;
    int tlsv1 = 0;

    while( (c = getopt( argc, argv, "c:e:k:d:hi:p:t:Tvf:n:o:r:" )) != -1 )
    {
        switch(c)
        {
            case 'h':
                fprintf(stderr, "-T\t\tTLS v1 protocol\n" );
                fprintf(stderr, "-t <host>\tTarget host name (default 'localhost')\n" );
                fprintf(stderr, "-p <port>\tTarget port number (default 16903)\n" );
                fprintf(stderr, "-c <file>\tCA certificate file\n" );
                fprintf(stderr, "-e <file>\tCertificate file\n" );
                fprintf(stderr, "-k <file>\tPrivate key file\n" );
                fprintf(stderr, "-d <dir>\tCA certificate directory\n" );
                fprintf(stderr, "-i <instruction>\tInstruction name\n");
                fprintf(stderr, "-o <path>\tOutput file path\n");
                fprintf(stderr, "-f <path>\tConfiguration file path\n");
                fprintf(stderr, "-n <name>\tProject name\n");
                fprintf(stderr, "-o <order>\tOrder of the project\n");
                fprintf(stderr, "-r <dir>\tDir to restore\n");
                fprintf(stderr, "-v\t\tVerbose\n" );
                exit(0);

            case 't':
                if ( ! (host = strdup( optarg )) )
                    err_exit( "Out of memory" );
                break;

            case 'p':
                if ( ! (port = strdup( optarg )) )
                    err_exit( "Invalid port specified" );
                break;
            case 'r':
                if ( ! (dir = strdup( optarg )) )
                    err_exit( "Invalid dir specified" );
                break;

            case 'd':
                if ( ! (cadir = strdup( optarg )) )
                    err_exit( "Out of memory" );
                break;

            case 'c':
                if ( ! (cafile = strdup( optarg )) )
                    err_exit( "Out of memory" );
                break;

            case 'e':   /* Certificate File */
                if ( ! (certfile = strdup( optarg )) )
                    err_exit( "Out of memory" );
                break;
            case 'i':
                if ( ! (instruction = strdup( optarg )) )
                    err_exit( "Out of memory");
                break;

            case 'k':
                if ( ! (keyfile = strdup( optarg )) )
                    err_exit( "Out of memory" );
                break;
            case 'n':
                if ( ! (prj_name = strdup( optarg )) )
                    err_exit( "Out of memory");
                break;
            
            case 'f':
                if(!(CFG_PATH = strdup(optarg)))
                    err_exit("Out of memory");
                break;
            
            case 'o':
                if( ! (order = strdup( optarg )) )
                    err_exit("Out of memory");
                break;
            case 'T':  tlsv1 = 1;       break;
            case 'v':  verbose = 1;     break;
        }
    }

    if(cafile == NULL)
        cafile = read_item("CA_certificate_file");
    if(certfile == NULL)
        certfile = read_item("Certificate_file");
    if(keyfile == NULL)
        keyfile = read_item("Private_key_file");
    if(host == NULL)
        host = read_item("Target_host");
    if(port == NULL)
        port = read_item("Target_port");

    /* Initialize SSL Library */
    SSL_library_init();
    SSL_load_error_strings();

    /* Error message output */
    bio_err = BIO_new_fp( stderr, BIO_NOCLOSE );

    /* Set up a SIGPIPE handler */
    signal( SIGPIPE, sigpipe_handle );

    /* Create SSL context*/
    if ( tlsv1 )
        meth = TLSv1_method();
    else
        meth = SSLv23_method();

    ctx = SSL_CTX_new( meth );

    /* Load the CAs we trust*/
    if ( (cafile || cadir)  &&
            ! SSL_CTX_load_verify_locations( ctx, cafile, cadir ) )
        ssl_err_exit( "Can't read CA list" );

    /* Load certificates */
    if ( certfile && ! SSL_CTX_use_certificate_chain_file( ctx, certfile ) )
        ssl_err_exit( "Can't read certificate file" );

    SSL_CTX_set_default_passwd_cb( ctx, password_cb );
    if ( keyfile )
    {
        fprintf(stderr, "load key file %s\n", keyfile);
        /* Load private key */
        if ( ! SSL_CTX_use_PrivateKey_file( ctx, keyfile, SSL_FILETYPE_PEM ) )
            ssl_err_exit( "Can't read key file" );
    }

    sock = ip_connect( SOCK_STREAM, IPPROTO_TCP, host, port );

    /* Associate SSL connection with server socket */
    ssl = SSL_new( ctx );
    sbio = BIO_new_socket( sock, BIO_NOCLOSE );
    SSL_set_bio( ssl, sbio, sbio );

    if ( verbose )
    {
        const char *str;
        int i;

        fprintf(stderr, "Ciphers: \n" );

        for( i = 0; (str = SSL_get_cipher_list( ssl, i )); i++ )
            fprintf( stderr, "    %s\n", str );
    }

    /* Perform SSL client connect handshake */
    if ( SSL_connect( ssl ) <= 0 )
        ssl_err_exit( "SSL connect error" );

    check_certificate( ssl, 1 );

    if ( verbose )
        fprintf(stderr, "Cipher: %s\n", SSL_get_cipher( ssl ) );

    /* Now make our request */
    //1.get all projects that can be restored
    //2.for each prj in projects get the backup_id and time_line
    //3.the user specify a specified prj_name and bacup_id to restore
    
    string ins(instruction);
    if(instruction)
    {
        //instruction = getinfo
        if(!(strcmp(ins.c_str(),"getinfo")))
        {
            vector<string> prj_list;
            size_t i = 0;
            restore_get_prj(ssl, prj_list);
            size_t size = prj_list.size();
            /*
            for(int loop = 0; loop < (int)size; loop++)
            {
                fprintf(stderr, "project name:%s\n",prj_list.at(loop).c_str());
            }
            */
            for(; i < size; i++)
            {
                restore_get_time_line(ssl, prj_list.at(i).c_str());
            }
        }
        //instruction = restore
        else if(!(strcmp(ins.c_str(),"restore")))
        {
            uint32_t tmp_id = atoi(order);
            if(dir)
            {
                string tmp_dir(dir);
                restore(ssl, prj_name, tmp_id, tmp_dir.c_str());
            }
            else
                fprintf(stderr, "Lack of dir to restore\n");
            //restore(ssl, prj_name, order, const char *prj_restore_dir)
        }
        else
            fprintf(stderr, "Can not find instruction\n");
    }
    else
        fprintf(stderr, "Input your instruction\n");

    /* Shutdown SSL connection */
    if(SSL_shutdown( ssl ) == 0)
    {
        shutdown(sock, SHUT_WR);
        if(SSL_shutdown(ssl) != 1)
            fprintf(stderr, "SSL_shutdown failed\n");
    }
    SSL_free( ssl );
    SSL_CTX_free(ctx);
    close( sock );

    exit(0);

}