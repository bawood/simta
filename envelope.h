/*
 * Copyright (c) 1998 Regents of The University of Michigan.
 * All Rights Reserved.  See COPYRIGHT.
 */

#ifdef __STDC__
#define ___P(x)		x
#else /* __STDC__ */
#define ___P(x)		()
#endif /* __STDC__ */

#define	R_DELIVERED	0
#define	R_FAILED	1
#define	R_TEMPFAIL	2

struct recipient {
    struct recipient	*r_next;
    char		*r_rcpt;
    int			r_delivered;
};

struct envelope {
    struct sockaddr_in	*e_sin;
    char		e_hostname[ MAXHOSTNAMELEN ];
    char		e_expanded[ MAXHOSTNAMELEN ];
    char		*e_helo;
    char		*e_mail;
    struct recipient	*e_rcpt;
    char		e_id[ 30 ];
    int			e_flags;
    int			e_success;
    int			e_failed;
    int			e_tempfail;
};

#define E_TLS		(1<<0)

void		env_reset ___P(( struct envelope * ));
void		env_stdout ___P(( struct envelope * ));
void		env_cleanup ___P(( struct envelope *e ));

/* return pointer on success, NULL on syserror, no syslog */
struct envelope	*env_create ___P(( char * ));
void		env_free ___P(( struct envelope * ));
void		rcpt_free ___P(( struct recipient * ));

/* return 0 on success, -1 on syserror, no syslog */
int		env_recipient ___P(( struct envelope *, char * ));
int		env_outfile ___P(( struct envelope *, char * ));

/* return 0 on success, -1 on syserror, 1 on syntax error, no syslog */
int		env_unexpanded ___P(( char *, int * ));
int		env_infile ___P(( struct envelope *, char * ));
