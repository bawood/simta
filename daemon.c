/*
 * Copyright (c) 1999 Regents of The University of Michigan.
 * All Rights Reserved.  See COPYRIGHT.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef TLS
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#endif /* TLS */

#include <snet.h>

#include "receive.h"
#include "ll.h"

#define _PATH_SPOOL	"/var/spool/simta"

int		debug = 0;
int		backlog = 5;

char			localhost[ MAXHOSTNAMELEN + 1 ];
struct stab_entry 	*hosts = NULL;

char		*maildomain = NULL;
char		*version = VERSION;

void		hup ___P(( int ));
void		chld ___P(( int ));
int		main ___P(( int, char *av[] ));

    void
hup( sig )
    int			sig;
{
    syslog( LOG_INFO, "reload %s", version );
}

    void
chld( sig )
    int			sig;
{
    int			pid, status;
    extern int		errno;

    while (( pid = waitpid( 0, &status, WNOHANG )) > 0 ) {
	if ( WIFEXITED( status )) {
	    if ( WEXITSTATUS( status )) {
		syslog( LOG_ERR, "child %d exited with %d", pid,
			WEXITSTATUS( status ));
	    } else {
		syslog( LOG_INFO, "child %d done", pid );
	    }
	} else if ( WIFSIGNALED( status )) {
	    syslog( LOG_ERR, "child %d died on signal %d", pid,
		    WTERMSIG( status ));
	} else {
	    syslog( LOG_ERR, "child %d died", pid );
	}
    }

    if ( pid < 0 && errno != ECHILD ) {
	syslog( LOG_ERR, "wait3: %m" );
	exit( 1 );
    }
    return;
}

SSL_CTX		*ctx = NULL;

    int
main( ac, av )
    int		ac;
    char	*av[];
{
    struct sigaction	sa, osahup, osachld;
    struct sockaddr_in	sin;
    struct servent	*se;
    int			c, s, err = 0, fd, sinlen;
    int			dontrun = 0;
    int			reuseaddr = 1;
    char		*prog;
    char		*spooldir = _PATH_SPOOL;
    char		*cryptofile = NULL;
    int			use_randfile = 0;
    unsigned short	port = 0;
    extern int		optind;
    extern char		*optarg;

    if (( prog = strrchr( av[ 0 ], '/' )) == NULL ) {
	prog = av[ 0 ];
    } else {
	prog++;
    }

    while (( c = getopt( ac, av, "C:rVcdp:b:M:s:" )) != -1 ) {
	switch ( c ) {
	case 'V' :		/* virgin */
	    printf( "%s\n", version );
	    exit( 0 );

	case 'C' :
	    cryptofile = optarg;
	    break;

	case 'r' :
	    use_randfile = 1;
	    break;

	case 'c' :		/* check config files */
	    dontrun++;
	    break;

	case 'd' :		/* debug */
	    debug++;
	    break;

	case 'p' :		/* TCP port */
	    port = htons( atoi( optarg ));
	    break;

	case 'b' :		/* listen backlog */
	    backlog = atoi( optarg );
	    break;

	case 'M' :
	    maildomain = optarg;
	    break;

	case 's' :		/* spool dir */
	    spooldir = optarg;
	    break;

	default :
	    err++;
	}
    }

    if ( err || optind != ac ) {
	fprintf( stderr,
		"Usage:\t%s [ -d ] [ -p port ] [ -b backlog ]\n",
		prog );
	exit( 1 );
    }

    /*
     * Read config file before chdir(), in case config file is relative path.
     */

    if ( gethostname( localhost, MAXHOSTNAMELEN + 1 ) !=0 ) {
	perror( "gethostname" );
	exit( 1 );
    }

    /* Add localhost to hosts list */
    if ( ll_insert( &hosts, localhost, NULL, NULL ) != 0 ) {
	perror( "ll_insert" );
	exit( 1 );
    }
    if ( ll_insert_tail( (struct stab_entry**)&(hosts->st_data),
	    "alias", "alias" ) != 0 ) {
	perror( "ll_insert_tail" );
	exit( 1 );
    }
    if ( ll_insert_tail( (struct stab_entry**)&(hosts->st_data),
	    "password", "password" ) != 0 ) {
	perror( "ll_insert_tail" );
	exit( 1 );
    }

    if ( chdir( spooldir ) < 0 ) {
	perror( spooldir );
	exit( 1 );
    }

    if ( cryptofile != NULL ) {
	SSL_load_error_strings();
	SSL_library_init();

	if ( use_randfile ) {
	    char        randfile[ MAXPATHLEN ];

	    if ( RAND_file_name( randfile, sizeof( randfile )) == NULL ) {
		fprintf( stderr, "RAND_file_name: %s\n",
			ERR_error_string( ERR_get_error(), NULL ));
		exit( 1 );
	    }
	    if ( RAND_load_file( randfile, -1 ) <= 0 ) {
		fprintf( stderr, "RAND_load_file: %s: %s\n", randfile,
			ERR_error_string( ERR_get_error(), NULL ));
		exit( 1 );
	    }
	    if ( RAND_write_file( randfile ) < 0 ) {
		fprintf( stderr, "RAND_write_file: %s: %s\n", randfile,
			ERR_error_string( ERR_get_error(), NULL ));
		exit( 1 );
	    }
	}

	if (( ctx = SSL_CTX_new( SSLv23_server_method())) == NULL ) {
	    fprintf( stderr, "SSL_CTX_new: %s\n",
		    ERR_error_string( ERR_get_error(), NULL ));
	    exit( 1 );
	}

	if ( SSL_CTX_use_PrivateKey_file( ctx, "CERT.pem", SSL_FILETYPE_PEM )
		!= 1 ) {
	    fprintf( stderr, "SSL_CTX_use_PrivateKey_file: %s: %s\n",
		    cryptofile, ERR_error_string( ERR_get_error(), NULL ));
	    exit( 1 );
	}
	if ( SSL_CTX_use_certificate_chain_file( ctx, "CERT.pem" ) != 1 ) {
	    fprintf( stderr, "SSL_CTX_use_certificate_chain_file: %s: %s\n",
		    cryptofile, ERR_error_string( ERR_get_error(), NULL ));
	    exit( 1 );
	}
	if ( SSL_CTX_check_private_key( ctx ) != 1 ) {
	    fprintf( stderr, "SSL_CTX_check_private_key: %s\n",
		    ERR_error_string( ERR_get_error(), NULL ));
	    exit( 1 );
	}

	if ( SSL_CTX_load_verify_locations( ctx, "CA.pem", NULL ) != 1 ) {
	    fprintf( stderr, "SSL_CTX_load_verify_locations: %s: %s\n",
		    cryptofile, ERR_error_string( ERR_get_error(), NULL ));
	    exit( 1 );
	}
	SSL_CTX_set_verify( ctx,
		SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL );
    }


    if ( dontrun ) {
	exit( 0 );
    }

    if ( port == 0 ) {
	if (( se = getservbyname( "smtp", "tcp" )) == NULL ) {
	    fprintf( stderr, "%s: can't find smtp service\n%s: continuing...\n",
		    prog, prog );
	    port = htons( 25 );
	} else {
	    port = se->s_port;
	}
    }

    /*
     * Set up listener.
     */
    if (( s = socket( PF_INET, SOCK_STREAM, 0 )) < 0 ) {
	perror( "socket" );
	exit( 1 );
    }
    if ( reuseaddr ) {
	if ( setsockopt( s, SOL_SOCKET, SO_REUSEADDR, (void*)&reuseaddr,
		sizeof( int )) < 0 ) {
	    perror("setsockopt");
	}
    }

    memset( &sin, 0, sizeof( struct sockaddr_in ));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = port;
    if ( bind( s, (struct sockaddr *)&sin, sizeof( struct sockaddr_in )) < 0 ) {
	perror( "bind" );
	exit( 1 );
    }
    if ( listen( s, backlog ) < 0 ) {
	perror( "listen" );
	exit( 1 );
    }

    /*
     * Disassociate from controlling tty.
     */
    if ( !debug ) {
	int		i, dt;

	switch ( fork()) {
	case 0 :
	    if ( setsid() < 0 ) {
		perror( "setsid" );
		exit( 1 );
	    }
	    dt = getdtablesize();
	    for ( i = 0; i < dt; i++ ) {
		if ( i != s ) {			/* keep socket open */
		    (void)close( i );
		}
	    }
	    if (( i = open( "/", O_RDONLY, 0 )) == 0 ) {
		dup2( i, 1 );
		dup2( i, 2 );
	    }
	    break;
	case -1 :
	    perror( "fork" );
	    exit( 1 );
	default :
	    exit( 0 );
	}
    }

    /*
     * Start logging.
     */
#ifdef ultrix
    openlog( prog, LOG_NOWAIT|LOG_PID );
#else /* ultrix */
    openlog( prog, LOG_NOWAIT|LOG_PID, LOG_SIMTA );
#endif /* ultrix */

    /* catch SIGHUP */
    memset( &sa, 0, sizeof( struct sigaction ));
    sa.sa_handler = hup;
    if ( sigaction( SIGHUP, &sa, &osahup ) < 0 ) {
	syslog( LOG_ERR, "sigaction: %m" );
	exit( 1 );
    }

    /* catch SIGCHLD */
    memset( &sa, 0, sizeof( struct sigaction ));
    sa.sa_handler = chld;
    if ( sigaction( SIGCHLD, &sa, &osachld ) < 0 ) {
	syslog( LOG_ERR, "sigaction: %m" );
	exit( 1 );
    }

    syslog( LOG_INFO, "restart %s", version );

    /*
     * Begin accepting connections.
     */
    for (;;) {
	/* should select() so we can manage an event queue */

	sinlen = sizeof( struct sockaddr_in );
	if (( fd = accept( s, (struct sockaddr *)&sin, &sinlen )) < 0 ) {
	    if ( errno != EINTR ) {	/* other errors? */
		syslog( LOG_ERR, "accept: %m" );
	    }
	    continue;
	}

	/* start child */
	switch ( c = fork()) {
	case 0 :
	    close( s );

	    /* reset CHLD and HUP */
	    if ( sigaction( SIGCHLD, &osachld, 0 ) < 0 ) {
		syslog( LOG_ERR, "sigaction: %m" );
		exit( 1 );
	    }
	    if ( sigaction( SIGHUP, &osahup, 0 ) < 0 ) {
		syslog( LOG_ERR, "sigaction: %m" );
		exit( 1 );
	    }

	    exit( receive( fd, &sin ));

	case -1 :
	    /*
	     * We don't tell the client why we're closing -- they will
	     * queue mail and try later.  We don't sleep() because we'd
	     * like to cause as much mail as possible to queue on remote
	     * hosts, thus spreading out load on our (memory bound) server.
	     */
	    close( fd );
	    syslog( LOG_ERR, "fork: %m" );
	    break;

	default :
	    close( fd );
	    syslog( LOG_INFO, "child %d for %s", c, inet_ntoa( sin.sin_addr ));
	    break;
	}
    }
}
