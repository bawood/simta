#ifdef __STDC__
#define ___P(x)		x
#else /* __STDC__ */
#define ___P(x)		()
#endif /* __STDC__ */

#include "config.h"

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/param.h>

#ifdef HAVE_LIBSSL
#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#endif /* HAVE_LIBSSL */

#include <fcntl.h>
#include <utime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <syslog.h>
#include <snet.h>

#include "denser.h"
#include "queue.h"
#include "envelope.h"
#include "expand.h"
#include "ll.h"
#include "simta.h"
#include "bounce.h"
#include "line_file.h"
#include "oklist.h"

int				simta_expand_debug = 0;


    /* return EXPAND_OK on success
     * return EXPAND_SYSERROR on syserror
     * return EXPAND_FATAL on fata errors (leaving fast files behind in error)
     * syslog errors
     */

    int
expand_and_deliver( struct host_q **hq_stab, struct envelope *unexpanded_env )
{
    syslog( LOG_DEBUG, "expand_and_deliver %s", unexpanded_env->e_id );

    switch ( expand( hq_stab, unexpanded_env )) {
    case 0:
	if ( q_runner( hq_stab ) != 0 ) {
	    syslog( LOG_ERR, "expand_and_deliver fast file fatal error" );
	    return( EXPAND_FATAL );
	}
	return( EXPAND_OK );

    default:
	syslog( LOG_ERR, "expand_and_deliver expand value out of range" );
    case 1:
    case -1:
	env_slow( unexpanded_env );
	if ( simta_fast_files > 0 ) {
	    syslog( LOG_ERR, "expand_and_deliver fast file fatal error" );
	    return( EXPAND_FATAL );
	}
	syslog( LOG_DEBUG, "expand_and_deliver returning syserror" );
	return( EXPAND_SYSERROR );
    }
}


#ifdef HAVE_LDAP
    void
exp_addr_prune( struct exp_addr *e_addr )
{
    if ( e_addr != NULL ) {
	e_addr->e_addr_status =
		( e_addr->e_addr_status & ( ~STATUS_TERMINAL ));
	exp_addr_prune( e_addr->e_addr_peer );
	exp_addr_prune( e_addr->e_addr_child );
    }
}
#endif /* HAVE_LDAP */


    /* return 0 on success
     * return 1 on syserror
     * return -1 on fata errors (leaving fast files behind in error)
     * syslog errors
     */

    int
expand( struct host_q **hq_stab, struct envelope *unexpanded_env )
{
    struct expand		exp;
    struct envelope		*base_error_env;
    struct envelope		*env_dead = NULL;
    struct envelope		*env;
    struct envelope		**env_p;
    struct recipient		*r;
    struct stab_entry		*i;
    struct stab_entry		*j;
    struct exp_addr		*e_addr;
    char			*domain;
    SNET			*snet = NULL;
    struct stab_entry		*host_stab = NULL;
    int				return_value = 1;
    int				fast_file_start;
    char			e_original[ MAXPATHLEN ];
    char			d_original[ MAXPATHLEN ];
    char			d_out[ MAXPATHLEN ];

    syslog( LOG_DEBUG, "expand %s", unexpanded_env->e_id );

    memset( &exp, 0, sizeof( struct expand ));
    exp.exp_env = unexpanded_env;
    fast_file_start = simta_fast_files;

    /* call address_expand on each address in the expansion list.
     *
     * if an address is expandable, the address(es) that it expands to will
     * be added to the expansion list. These non-terminal addresses must
     * have their st_data set to NULL to specify that they are not to be
     * included in the terminal expansion list. 
     *
     * Any address in the expansion list who's st_data is not NULL is
     * considered a terminal address and will be written out as one
     * of the addresses in expanded envelope(s).
     */ 

    /* set our iterator to NULL to signify we're at the start of the loop */
    i = NULL;

    if (( base_error_env = address_bounce_create( &exp )) == NULL ) {
	syslog( LOG_ERR, "expand.env_create: %m" );
	return( 1 );
    }

    if ( env_recipient( base_error_env, unexpanded_env->e_mail ) != 0 ) {
	syslog( LOG_ERR, "expand.env_recipient: %m" );
	return( 1 );
    }

    for ( r = unexpanded_env->e_rcpt; r != NULL; r = r->r_next ) {
	/* Add ONE address from the original envelope's rcpt list */
	/* this address has no parent, it is a "root" address because
	 * it's a rcpt.
	 */

#ifdef HAVE_LDAP
	exp.exp_parent = NULL;
#endif /* HAVE_LDAP */

	if ( add_address( &exp, r->r_rcpt, base_error_env, ADDRESS_TYPE_EMAIL )
		!= 0 ) {
	    /* add_address syslogs errors */
	    goto cleanup1;
	}

	for ( ; ; ) {
	    if ( i == NULL ) {
		/* we need to start by processing the first addr in the
		 * expansion structure.
		 */
		i = exp.exp_addr_list;
	    } else if ( i->st_next == NULL ) {
		/* there are no more address for processing at this time */
		break;
	    } else {
		/* process the next address */
		i = i->st_next;
	    }

	    e_addr = (struct exp_addr*)i->st_data;

#ifdef HAVE_LDAP
	    exp.exp_parent = e_addr;
#endif /* HAVE_LDAP */

	    switch ( address_expand( &exp, e_addr )) {
	    case ADDRESS_EXCLUDE:
		e_addr->e_addr_status =
			( e_addr->e_addr_status & ( ~STATUS_TERMINAL ));
		/* the address is not a terminal local address */
		break;

	    case ADDRESS_FINAL:
		e_addr->e_addr_status =
			( e_addr->e_addr_status | STATUS_TERMINAL );
		/* the address is a terminal local address */
		break;

	    default:
		syslog( LOG_ERR,
			"expand.address_expand: out of bounds return" );
	    case ADDRESS_SYSERROR:
		goto cleanup1;
	    }
	}
    }

    /* Create one expanded envelope for every host we expanded address for */
    for ( i = exp.exp_addr_list; i != NULL; i = i->st_next ) {
	e_addr = (struct exp_addr*)i->st_data;

#ifdef HAVE_LDAP
	/* prune exclusive groups the sender is not a member of */
	if ((( e_addr->e_addr_status & STATUS_EMAIL_SENDER ) == 0 ) &&
		(( e_addr->e_addr_status & STATUS_LDAP_EXCLUSIVE ) != 0 )) {
	    exp_addr_prune( e_addr->e_addr_child );
	}
#endif /* HAVE_LDAP */

	if (( e_addr->e_addr_status & STATUS_TERMINAL ) == 0 ) {
	    /* not a terminal expansion, do not add */
	    continue;
	}

	if ( e_addr->e_addr_type == ADDRESS_TYPE_EMAIL ) {
	    if (( domain = strchr( i->st_key, '@' )) == NULL ) {
		syslog( LOG_ERR, "expand.strchr: unreachable code" );
		goto cleanup2;
	    }
	    domain++;
	    env = (struct envelope*)ll_lookup( host_stab, domain );

	} else if ( e_addr->e_addr_type == ADDRESS_TYPE_DEAD ) {
	    domain = NULL;
	    env = env_dead;

	} else {
	    /* not a terminal expansion, do not add */
	    syslog( LOG_WARNING, "expand unknown address type output" );
	    continue;
	}

	if ( env == NULL ) {
	    /* Create envelope and add it to list */
	    if (( env = env_create( unexpanded_env->e_mail )) == NULL ) {
		syslog( LOG_ERR, "expand.env_create: %m" );
		goto cleanup2;
	    }

	    if ( env_gettimeofday_id( env ) != 0 ) {
		env_free( env );
		goto cleanup2;
	    }

	    /* fill in env */
	    if ( domain != NULL ) {
		env->e_dir = simta_dir_fast;
		strcpy( env->e_hostname, domain );
	    } else {
		env->e_dir = simta_dir_dead;
	    }

	    /* Add env to host_stab */
	    if ( ll_insert( &host_stab, env->e_hostname, env, NULL ) != 0 ) {
		syslog( LOG_ERR, "expand.ll_insert: %m" );
		env_free( env );
		goto cleanup2;
	    }
	}

	if ( env_recipient( env, i->st_key ) != 0 ) {
	    goto cleanup2;
	}
    }

    sprintf( d_original, "%s/D%s", unexpanded_env->e_dir,
	    unexpanded_env->e_id );

    /* Write out all expanded envelopes and place them in to the host_q */
    for ( i = host_stab; i != NULL; i = i->st_next ) {
	env = i->st_data;

	if ( simta_expand_debug == 0 ) {
	    /* Dfile: link Dold_id env->e_dir/Dnew_id */
	    sprintf( d_out, "%s/D%s", env->e_dir, env->e_id );

	    if ( link( d_original, d_out ) != 0 ) {
		syslog( LOG_ERR, "expand: link %s %s: %m", d_original, d_out );
		goto cleanup3;
	    }

	    /* Efile: write env->e_dir/Enew_id for all recipients at host */
	    syslog( LOG_INFO, "expand %s: writing %s %s", unexpanded_env->e_id,
		    env->e_id, env->e_hostname );
	    if ( env_outfile( env ) != 0 ) {
		/* env_outfile syslogs errors */
		if ( unlink( d_out ) != 0 ) {
		    syslog( LOG_ERR, "expand unlink %s: %m", d_out );
		}
		goto cleanup3;
	    }

	    queue_envelope( hq_stab, env );

	} else {
	    env_stdout( env );
	    printf( "\n" );
	}
    }

    /* XXX if ( expanded_out == 0 ) { mail loop? } */

    /* write errors out to disk */
    env_p = &(exp.exp_errors);
    while (( env = *env_p ) != NULL ) {
	if ( simta_expand_debug == 0 ) {
	    if ( env->e_err_text != NULL ) {
		env_p = &(env->e_next);

		if ( env == base_error_env ) {
		    /* send the message back to the original sender */
		    if (( snet = snet_open( d_original, O_RDWR, 0,
			    1024 * 1024 )) == NULL ) {
			syslog( LOG_ERR, "expand.snet_open %s: %m",
				d_original );
			goto cleanup4;
		    }
		}

		/* write out error text, get Dfile inode */
		if (( env->e_dinode = bounce_dfile_out( env, snet )) == 0 ) {
		    if ( snet != NULL ) {
			if ( snet_close( snet ) != 0 ) {
			    syslog( LOG_ERR, "expand.snet_close %s: %m",
				    d_original );
			}
		    }

		    goto cleanup4;
		}

		line_file_free( env->e_err_text );
		env->e_err_text = NULL;

		if ( snet != NULL ) {
		    if ( snet_close( snet ) != 0 ) {
			syslog( LOG_ERR, "expand.snet_close %s: %m",
				d_original );
			sprintf( d_out, "%s/D%s", env->e_dir, env->e_id );
			if ( unlink( d_out ) != 0 ) {
			    syslog( LOG_ERR, "expand unlink %s: %m", d_out );
			}
			goto cleanup4;
		    }
		    snet = NULL;
		}

		syslog( LOG_INFO, "expand %s: writing bounce %s %s",
			unexpanded_env->e_id, env->e_id, env->e_hostname );
		env->e_dir = simta_dir_fast;
		if ( env_outfile( env ) != 0 ) {
		    /* env_outfile syslogs errors */
		    sprintf( d_out, "%s/D%s", env->e_dir, env->e_id );
		    if ( unlink( d_out ) != 0 ) {
			syslog( LOG_ERR, "expand unlink %s: %m", d_out );
		    }
		    goto cleanup4;
		}

		queue_envelope( hq_stab, env );

	    } else {
		*env_p = env->e_next;
		env_free( env );
	    }

	} else {
	    env_p = &(env->e_next);
	    bounce_stdout( env );
	}
    }

    if ( simta_expand_debug != 0 ) {
	return_value = 0;
	goto cleanup2;
    }

    /* trunacte & delete unexpanded message */
    sprintf( e_original, "%s/E%s", unexpanded_env->e_dir,
	    unexpanded_env->e_id );

    if ( unexpanded_env->e_dir != simta_dir_fast ) {
	if ( truncate( e_original, (off_t)0 ) != 0 ) {
	    syslog( LOG_ERR, "expand.truncate %s: %m", e_original );
	    goto cleanup4;
	}
    }

    if ( env_unlink( unexpanded_env ) != 0 ) {
	syslog( LOG_ERR, "expand env_unlink %s: can't delete original message",
		unexpanded_env->e_id );
    }

    return_value = 0;
    goto cleanup2;

cleanup4:
    while ( exp.exp_errors != NULL ) {
	env = exp.exp_errors;
	exp.exp_errors = exp.exp_errors->e_next;

	/* unlink if written to disk */
	if (( env->e_flags & ENV_ON_DISK ) != 0 ) {
	    queue_remove_envelope( env );
	    env_unlink( env );
	}

	env_free( env );
    }

cleanup3:
    for ( i = host_stab; i != NULL; i = i->st_next ) {
	env = i->st_data;

	if (( env->e_flags & ENV_ON_DISK ) != 0 ) {
	    queue_remove_envelope( env );
	    env_unlink( env );
	}

	env_free( env );
	i->st_data = NULL;
	i = i->st_next;
    }

    if ( simta_fast_files != fast_file_start ) {
	return( -1 );
    }

cleanup2:
    /* free host_stab */
    i = host_stab;
    while ( i != NULL ) {
	j = i;
	i = i->st_next;
	free( j );
    }

cleanup1:
    /* free exp_addr_list */
    i = exp.exp_addr_list;
    while ( i != NULL ) {
	j = i;
	i = i->st_next;
	if ( j->st_data != NULL ) {
	    e_addr = (struct exp_addr*)j->st_data;
#ifdef HAVE_LDAP  
	    ok_destroy (e_addr);
	    if (e_addr->e_addr_dn )
		free (e_addr->e_addr_dn);
#endif
	    free( e_addr->e_addr );
	    free( e_addr );
	}
	free( j );
    }

    return( return_value );
}


#ifdef HAVE_LDAP
    int
ldap_check_ok( struct expand *exp, struct exp_addr *exclusive_addr )
{
    struct exp_addr		*parent;
    struct recipient		*r;

    void			*match;

    /* XXX this should also check to see if exclusive_addr has an ok list */
    if ( exclusive_addr == NULL ) {
	return( 0 );
    }

    parent = exp->exp_parent;

    while ( parent->e_addr_type != ADDRESS_TYPE_EMAIL ) {
	parent = parent->e_addr_parent;
    }

    for ( r = exp->exp_env->e_rcpt; r != NULL; r = r->r_next ) {
	if ( strcasecmp( r->r_rcpt, parent->e_addr ) == 0 ) {
	    break;
	}
    }

    if ( r == NULL ) {
	return( 0 );
    }

    match = ll_lookup( exclusive_addr->e_addr_ok, parent->e_addr_dn );
    
    return( match ? 1 : 0 );
}
#endif /* HAVE_LDAP */
