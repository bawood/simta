/**********          header.c          **********/

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <time.h>

#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <netdb.h>
#include <pwd.h>
#include <unistd.h>
#include <stdlib.h>

#include "line_file.h"
#include "envelope.h"
#include "header.h"
#include "receive.h"
#include "simta.h"

#define	TOKEN_UNDEFINED		0
#define	TOKEN_QUOTED_STRING	1
#define	TOKEN_DOT_ATOM		2
#define	TOKEN_DOMAIN_LITERAL	3


struct line_token {
    int			t_type;
    char		*t_start;
    struct line		*t_start_line;
    char		*t_end;
    struct line		*t_end_line;
};


int	skip_cfws ___P(( struct line **, char ** ));
int	is_dot_atom_text ___P(( int ));
int	line_token_da ___P(( struct line_token *, struct line *, char * ));
int	line_token_qs ___P(( struct line_token *, struct line *, char * ));
int	line_token_dl ___P(( struct line_token *, struct line *, char * ));
int	email_addr ___P(( struct line **, char ** ));
int	header_mbox_correct ___P(( struct line *, char * ));
int	header_from_correct ___P(( struct line_file * ));


struct header simta_headers[] = {
    { "Date",			NULL,		NULL },
#define HEAD_DATE		0
    { "From",			NULL,		NULL },
#define HEAD_FROM		1
    { "Sender",			NULL,		NULL },
#define HEAD_SENDER		2
    { "To",			NULL,		NULL },
#define HEAD_TO			3
    { "Message-ID",		NULL,		NULL },
#define HEAD_MESSAGE_ID		4
    { "Cc",			NULL,		NULL },
#define HEAD_CC			5
    { "Bcc",			NULL,		NULL },
#define HEAD_BCC		6
    { "Reply-To",		NULL,		NULL },
#define HEAD_REPLY_TO		7
    { "References",		NULL,		NULL },
#define HEAD_REFRENCES		8
    { "Subject",		NULL,		NULL },
#define HEAD_SUBJECT		9
    { NULL,			NULL,		NULL }
};


    /* 
     * return non-zero if the headers can't be uncommented
     * return 0 on success
     *	-c will be on next word, or NULL
     *	-l will be on c's line, or NULL
     */

    int
skip_cfws( struct line **l, char **c )
{
    int				comment = 0;

    for ( ; ; ) {
	switch ( **c ) {
	case ' ':
	case '\t':
	    break;

	case '(':
	    comment++;
	    break;

	case ')':
	    comment --;

	    if ( comment < 0 ) {
		return( -1 );
	    }
	    break;

	case '\\':
	    (*c)++;

	    if ( *c != '\0' ) {
		/* XXX should a trailing '\' be illegal? */
	    	break;
	    }

	case '\0':
	    /* end of line.  if next line starts with WSP, continue */
	    *l = (*l)->line_next;

	    if (( *l != NULL ) &&
		    (( *((*l)->line_data) == ' ' ) ||
		    ( *((*l)->line_data) == '\t' ))) {
		*c = (*l)->line_data;
		break;

	    } else {
		/* End of header */
		*c = NULL;
		*l = NULL;
		return( comment );
	    }

	default:
	    if ( comment == 0 ) {
		return( 0 );
	    }
	}

	(*c)++;
    }
}


    void
header_stdout( struct header h[])
{
    while ( h->h_key != NULL ) {
	/* print key */
	printf( "%s: ", h->h_key );

	if ( h->h_line != NULL ) {
	    printf( "%s", h->h_line->line_data );

	    if ( h->h_data != NULL ) {
		printf( "\n%s data: %s", h->h_key, h->h_data );
	    }

	} else {
	    printf( "NULL" );
	}

	printf( "\n" );

	h++;
    }
}



    /* Some mail clents exhibit bad behavior when generating headers.
     *
     * return 0 if all went well.
     * return 1 if we reject the message.
     * die -1 if there was a serious error.
     */

    int
header_exceptions( struct line_file *lf )
{
    char		*c;
    char		*end;

    if ( lf->l_first == NULL ) {
	/* empty message */
	return( 0 );
    }

    /* mail(1) on Solaris gives non-rfc compliant first header line */
    c = lf->l_first->line_data;

    if ( strncasecmp( c, "From ", 5 ) == 0 ) {
	c += 5;
	for ( end = c; ( *end > 33 ) && ( *end < 126 ); end++ )
		;

	/* if "From "word ..., rewrite header "From:"word'\0' */
	if (( end - c ) > 0 ) {
	    *(lf->l_first->line_data + 4) = ':';
	    *end = '\0';
	}
    }

    return( 0 );
}


    int
header_file_out( struct line_file *lf, FILE *file )
{
    struct line			*l;

    for ( l = lf->l_first; l != NULL; l = l->line_next ) {
	fprintf( file, "%s\n", l->line_data );
    }

    return( 0 );
}


    int
header_timestamp( struct envelope *env, FILE *file )
{
    struct sockaddr_in		sin;
    time_t			clock;
    struct tm			*tm;
    char			daytime[ 30 ];

    if ( env->e_sin != NULL ) {
	memcpy( &sin, env->e_sin, sizeof( struct sockaddr_in )); 

    } else {
	/* XXX local IP addr? */
	memset( &sin, 0, sizeof( struct sockaddr_in ));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = INADDR_ANY;
    }

    if ( time( &clock ) < 0 ) {
	return( -1 );
    }

    if (( tm = localtime( &clock )) == NULL ) {
	return( -1 );
    }

    if ( strftime( daytime, sizeof( daytime ), "%e %b %Y %T", tm ) == 0 ) {
	return( -1 );
    }

    /* Received header */
    if ( fprintf( file, "Received: FROM %s ([%s])\n\tBY %s ID %s ;\n\t%s %s\n",
	    env->e_mail, inet_ntoa( sin.sin_addr ), env->e_hostname,
	    env->e_id, daytime, tz( tm )) < 0 ) {
	return( -1 );
    }

    return( 0 );
}


    /* return 0 if line is the next line in header block lf */
    /* rfc2822, 2.1 General Description:
     * A message consists of header fields (collectively called "the header
     * of the message") followed, optionally, by a body.  The header is a
     * sequence of lines of characters with special syntax as defined in
     * this standard. The body is simply a sequence of characters that
     * follows the header and is separated from the header by an empty line
     * (i.e., a line with nothing preceding the CRLF).
     */

    /* rfc 2822, 2.2.2. Structured Header Field Bodies:
     * SP, ASCII value 32) and horizontal tab (HTAB, ASCII value 9)
     * characters (together known as the white space characters, WSP
     */

    int
header_end( struct line_file *lf, char *line )
{
    char		*c;

    /* null line means that message data begins */
    if (( *line ) == '\0' ) {
	return( 1 );
    }

    if (( *line == ' ' ) || ( *line == '\t' )) {

	/* line could be FWS if it's not the first line */
	if ( lf->l_first != NULL ) {
	    return( 0 );
	}

    } else {

	/* if line syntax is a header, return 0 */
	for ( c = line; *c != ':'; c++ ) {
	    /* colon ascii value is 58 */
	    if (( *c < 33 ) || ( *c > 126 )) {
		break;
	    }
	}

	if (( *c == ':' ) && (( c - line ) > 0 )) {
	    /* proper field name followed by a colon */
	    return( 0 );
	}
    }

    return( 1 );
}


    /* return 0 if all went well.
     * return 1 if we reject the message.
     * return -1 if there was a serious error.
     */

    /* all errors out to stderr, as you should only be correcting headers
     * from simsendmail, for now.
     */

    int
header_correct( struct line_file *lf, struct envelope *env )
{
    struct line			*l;
    struct line			**lp;
    struct header		*h;
    char			*colon;
    size_t			header_len;
    int				result;
    char			*sender;
    char			*prepend_line = NULL;
    size_t			prepend_len = 0;
    size_t			len;
    time_t			clock;
    struct tm			*tm;
    char			daytime[ 35 ];

    /* check headers for known mail clients behaving badly */
    if (( result = header_exceptions( lf )) != 0 ) {
	fprintf( stderr, "header_exceptions error\n" );
	return( result );
    }

    /* put header information in to data structures for later processing */
    for ( l = lf->l_first; l != NULL ; l = l->line_next ) {

	/* rfc 2822:
	 * Header fields are lines composed of a field name, followed
	 * by a colon (":"), followed by a field body, and terminated
	 * by CRLF.  A field name MUST be composed of printable
	 * US-ASCII characters (i.e., characters that have values
	 * between 33 and 126, inclusive), except colon.
	 */

	/* line is FWS if first character of the line is whitespace */
	if (( *l->line_data == ' ' ) || ( *l->line_data == '\t' )) {
	    continue;
	}

	for ( colon = l->line_data; *colon != ':'; colon++ )
		;

	header_len = ( colon - ( l->line_data ));

	/* field name followed by a colon */
	for ( h = simta_headers; h->h_key != NULL; h++ ) {
	    if ( strncasecmp( h->h_key, l->line_data, header_len ) == 0 ) {
		/* correct field name */
		if ( h->h_line == NULL ) {
		    h->h_line = l;

		} else {
		    /* header h->h_key appears at least twice */
		    fprintf( stderr, "Illegal duplicate header: %s\n",
			    h->h_key );
		    return( 1 );
		}
	    }
	}
    }

    if (( sender = simta_sender()) == NULL ) {
	return( -1 );
    }

    /* examine & correct header data */

    /* From: */
    if (( result = header_from_correct( lf )) != 0 ) {
	return( result );
    }

    /* XXX Sender */

    if ( simta_headers[ HEAD_DATE ].h_line == NULL ) {
	if ( time( &clock ) < 0 ) {
	    perror( "time" );
	    return( -1 );
	}

	if (( tm = localtime( &clock )) == NULL ) {
	    perror( "localtime" );
	    return( -1 );
	}

	if ( strftime( daytime, sizeof( daytime ), "%a, %e %b %Y %T", tm )
		== 0 ) {
	    perror( "strftime" );
	    return( -1 );
	}

	if (( len = ( strlen( simta_headers[ HEAD_DATE ].h_key ) +
		strlen( daytime ) + 3 )) > prepend_len ) {

	    if (( prepend_line = (char*)realloc( prepend_line, len ))
		    == NULL ) {
		perror( "realloc" );
		return( -1 );
	    }

	    prepend_len = len;
	}

	sprintf( prepend_line, "%s: %s",
		simta_headers[ HEAD_DATE ].h_key, daytime );

	if (( simta_headers[ HEAD_DATE ].h_line =
		line_prepend( lf, prepend_line )) == NULL ) {
	    perror( "malloc" );
	    return( -1 );
	}
    }

    if ( simta_headers[ HEAD_MESSAGE_ID ].h_line == NULL ) {
	if (( len = ( strlen( simta_headers[ HEAD_MESSAGE_ID ].h_key ) +
		strlen( env->e_id ) + 3 )) > prepend_len ) {
	    if (( prepend_line = (char*)realloc( prepend_line, len ))
		    == NULL ) {
		perror( "realloc" );
		return( -1 );
	    }

	    prepend_len = len;
	}

	sprintf( prepend_line, "%s: %s",
		simta_headers[ HEAD_MESSAGE_ID ].h_key, env->e_id );

	if (( simta_headers[ HEAD_MESSAGE_ID ].h_line =
		line_prepend( lf, prepend_line )) == NULL ) {
	    perror( "malloc" );
	    return( -1 );
	}
    }

    if ( simta_headers[ HEAD_TO ].h_line != NULL ) {
	/* XXX add to recipients if no -t flag? */
	/* To: blah: woof woof; */
    }

    if ( simta_headers[ HEAD_CC ].h_line != NULL ) {
	/* XXX add cc recipients if no -t flag? */
    }

    if (( l = simta_headers[ HEAD_BCC ].h_line ) != NULL ) {
	/* XXX add bcc recipients if no -t flag? */

	/* remove bcc lines */
	if ( l->line_prev != NULL ) {
	    lp = &(l->line_prev->line_next);

	} else {
	    lp = &(lf->l_first);
	}

	for ( l = l->line_next; l != NULL; l = l->line_next ) {
	    if (( *(l->line_data) != ' ' ) && ( *(l->line_data) != '\t' )) {
		break;
	    }
	}

	*lp = l;
    }

#ifdef DEBUG
    header_stdout( simta_headers );
#endif /* DEBUG */

    if ( prepend_line != NULL ) {
	free( prepend_line );
    }

    return( 0 );
}


    /* return 0 if all went well.
     * return 1 if we reject the message.
     * return -1 if there was a serious error.
     */

    /* all errors out to stderr, as you should only be correcting headers
     * from simsendmail, for now.
     */

    /* RFC 2822:
     *
     * mailbox-list    =   (mailbox *("," mailbox))
     * mailbox         =   name-addr / addr-spec
     * name-addr       =   [display-name] angle-addr
     * display-name    =   phrase
     * phrase          =   1*word
     * angle-addr      =   [CFWS] "<" addr-spec ">" [CFWS]
     * addr-spec       =   local-part "@" domain
     * local-part      =   dot-atom / quoted-string
     * domain          =   dot-atom / domain-literal
     * domain-literal  =   [CFWS] "[" *([FWS] dcontent) [FWS] "]" [CFWS]
     * word            =   atom / quoted-string
     * atom            =   [CFWS] 1*atext [CFWS]
     * atext           =   ALPHA / DIGIT / ; Any character except controls,
     *			     "!" / "#" /     ;  SP, and specials.
     *			     "$" / "%" /     ;  Used for atoms
     *			     "&" / "'" /
     *			     "*" / "+" /
     *			     "-" / "/" /
     *			     "=" / "?" /
     *			     "^" / "_" /
     *			     "`" / "{" /
     *			     "|" / "}" /
     *			     "~"
     * dcontent        =   dtext / quoted-pair
     * dtext           =   NO-WS-CTL /     ; Non white space controls
     *			%d33-90 /       ; The rest of the US-ASCII
     *			%d94-126        ;  characters not including "[",
     *					;  "]", or "\"
     * dot-atom        =   [CFWS] dot-atom-text [CFWS]
     * dot-atom-text   =   1*atext *("." 1*atext)
     * qtext           =       NO-WS-CTL /   ; Non white space controls
     *			    %d33 /       ; The rest of the US-ASCII
     *			    %d35-91 /    ;  characters not including "\"
     *			    %d93-126     ;  or the quote character
     * qcontent        =       qtext / quoted-pair
     * quoted-string   =       [CFWS]
     *			    DQUOTE *([FWS] qcontent) [FWS] DQUOTE
     *			    [CFWS]
     */


    int
email_addr( struct line **start_line, char **start )
{
    char				*next_c;
    struct line				*next_l;
    struct line_token			local;
    struct line_token			domain;
    int					result;

    if ( **start == '<' ) {
	next_c = (*start) + 1;

    } else {
	next_c = *start;
    }

    next_l = *start_line;

    if (( result = skip_cfws( &next_l, &next_c )) != 0 ) {
	if ( result > 0 ) {
	    fprintf( stderr, "Header From: unbalanced \(\n" );
	} else {
	    fprintf( stderr, "Header From: unbalanced )\n" );
	}
	return( 1 );
    }

    if ( next_c == NULL ) {
	fprintf( stderr, "Header From: address expected\n" );
	return( 1 );

    } else if ( *next_c == '"' ) {
	if ( line_token_qs( &local, next_l, next_c ) != 0 ) {
	    fprintf( stderr, "Header From: unbalanced \"\n" );
	    return( 1 );
	}

    } else {
	if ( line_token_da( &local, next_l, next_c ) != 0 ) {
	    fprintf( stderr, "Header From: bad token: %s\n", next_c );
	    return( 1 );
	}
    }

    next_c = local.t_end + 1;
    next_l = local.t_end_line;

    if (( result = skip_cfws( &next_l, &next_c )) != 0 ) {
	if ( result > 0 ) {
	    fprintf( stderr, "Header From: unbalanced \(\n" );
	} else {
	    fprintf( stderr, "Header From: unbalanced )\n" );
	}
	return( 1 );
    }

    if (( next_c == NULL ) || ( *next_c != '@' )) {
	fprintf( stderr, "Header From: '@' expected\n" );
	return( 1 );
    }

    next_c++;

    if (( result = skip_cfws( &next_l, &next_c )) != 0 ) {
	if ( result > 0 ) {
	    fprintf( stderr, "Header From: unbalanced \(\n" );
	} else {
	    fprintf( stderr, "Header From: unbalanced )\n" );
	}
	return( 1 );
    }

    if ( next_c == NULL ) {
	fprintf( stderr, "Header From: domain expected\n" );
	return( 1 );

    } else if ( *next_c == '[' ) {
	if ( line_token_dl( &domain, next_l, next_c ) != 0 ) {
	    fprintf( stderr, "Header From: unmatched [\n" );
	    return( 1 );
	}

    } else {
	if ( line_token_da( &domain, next_l, next_c ) != 0 ) {
	    fprintf( stderr, "Header From: bad token: %s\n", next_c );
	    return( 1 );
	}
    }

    next_c = domain.t_end + 1;
    next_l = domain.t_end_line;

    if (( result = skip_cfws( &next_l, &next_c )) != 0 ) {
	if ( result > 0 ) {
	    fprintf( stderr, "Header From: unbalanced \(\n" );
	} else {
	    fprintf( stderr, "Header From: unbalanced )\n" );
	}
	return( 1 );
    }

    if ( **start == '<' ) {
	if (( next_c == NULL ) || ( *next_c != '>' )) {
	    fprintf( stderr, "Header From: > expected\n" );
	    return( 1 );
	}

	next_c++;

	if (( result = skip_cfws( &next_l, &next_c )) != 0 ) {
	    if ( result > 0 ) {
		fprintf( stderr, "Header From: unbalanced \(\n" );
	    } else {
		fprintf( stderr, "Header From: unbalanced )\n" );
	    }
	    return( 1 );
	}
    }

    if ( next_c != NULL ) {
	if ( *next_c != ',' ) {
	    fprintf( stderr, "Header From: illegal words after address\n" );
	    return( 1 );
	}

	next_c++;

	if (( result = skip_cfws( &next_l, &next_c )) != 0 ) {
	    if ( result > 0 ) {
		fprintf( stderr, "Header From: unbalanced \(\n" );
	    } else {
		fprintf( stderr, "Header From: unbalanced )\n" );
	    }
	    return( 1 );
	}
    }

    *start = next_c;
    *start_line = next_l;

    return( 0 );
}


    int
header_mbox_correct( struct line *l, char *c )
{
    char				*next_c;
    struct line				*next_l;
    struct line_token			local;
    int					result;

    for ( ; ; ) {

	/*
	 * START: ( NULL )->NULL_ADDR
	 * START: ( .A | QS )->LOCAL_PART
	 * START: ( < )->AA_LEFT
	 */

	if ( c == NULL ) {
	    /* XXX NULL_ADDR */
	    fprintf( stderr, "Header From: NULL address\n" );
	    return( 1 );

	} else if ( *c != '<' ) {

	    /*
	     * LOCAL_PART: ( QS | DA ) ( NULL | , | @ ) -> SINGLE_ADDR
	     * LOCAL_PART: ( QS | DA ) ( < ) -> AA_LEFT
	     * LOCAL_PART: ( QS | DA ) ( .A | QS ) -> DISPLAY_NAME
	     */

	    if ( *c == '"' ) {
		if ( line_token_qs( &local, l, c ) != 0 ) {
		    fprintf( stderr, "Header From: unbalanced \"\n" );
		    return( 1 );
		}

	    } else {
		if ( line_token_da( &local, l, c ) != 0 ) {
		    fprintf( stderr, "Header From: bad token: %s\n", c );
		    return( 1 );
		}
	    }

	    next_c = local.t_end + 1;
	    next_l = local.t_end_line;

	    if (( result = skip_cfws( &next_l, &next_c )) != 0 ) {
		if ( result > 0 ) {
		    fprintf( stderr, "Header From: unbalanced \(\n" );
		} else {
		    fprintf( stderr, "Header From: unbalanced )\n" );
		}
		return( 1 );
	    }
	
	    if (( next_c == NULL ) || ( *next_c == ',' ) ||
		    ( *next_c == '@' )) {

		/* SINGLE_ADDR: email_addr ( NULL ) -> SINGLE_ADDR_DONE )
		 * SINGLE_ADDR: email_addr [ ( , ) -> START ] )
		 */
	
		c = local.t_start;
		l = local.t_start_line;

		if (( result = email_addr( &l, &c )) != 0 ) {
		    return( result );
		}
	
		if ( c == NULL ) {
		    /* SINGLE_ADDR_DONE */
		    return( 0 );
		}

		/* START */
		continue;

	    } else  {
		while ( *next_c != '<' ) {

		    /*
		     * DISPLAY_NAME: ( QS | DA )* ( < ) -> AA_LEFT
		     */

		    if ( *next_c == '"' ) {
			if ( line_token_qs( &local, next_l, next_c ) != 0 ) {
			    fprintf( stderr, "Header From: unbalanced \"\n" );
			    return( 1 );
			}

		    } else {
			if ( line_token_da( &local, next_l, next_c ) != 0 ) {
			    fprintf( stderr, "Header From: bad token: %s\n",
				    next_c);
			    return( 1 );
			}
		    }

		    next_c = local.t_end + 1;
		    next_l = local.t_end_line;

		    if (( result = skip_cfws( &next_l, &next_c )) != 0 ) {
			if ( result > 0 ) {
			    fprintf( stderr, "Header From: unbalanced \(\n" );
			} else {
			    fprintf( stderr, "Header From: unbalanced )\n" );
			}
			return( 1 );
		    }
		}
	    }

	    /* set c, l, fall through to AA_LEFT */
	    c = next_c;
	    l = next_l;
	}

	/*
	 * AA_LEFT: email_addr ( NULL ) -> AA_LEFT_DONE )
	 * AA_LEFT: email_addr [ ( , ) -> START ] )
	 */

	if (( result = email_addr( &l, &c )) != 0 ) {
	    return( result );
	}

	if ( c == NULL ) {
	    /* AA_LEFT_DONE */
	    return( 0 );
	}
    }
}


    int
header_from_correct( struct line_file *lf )
{
    struct line			*l;
    char			*c;
    struct line			*next_l;
    char			*next_c;
    int				result;
    char			*sender;
    char			*prepend_line = NULL;
    size_t			prepend_len = 0;
    size_t			len;

    if (( sender = simta_sender()) == NULL ) {
	return( -1 );
    }

    /* From: */
    if (( l = simta_headers[ HEAD_FROM ].h_line ) != NULL ) {
	c = l->line_data + 5;

	next_c = c;
	next_l = l;

	/* is there data on the line? */
	if (( result = skip_cfws( &next_l, &next_c )) != 0 ) {
	    if ( result > 0 ) {
		fprintf( stderr, "From: unbalanced \(\n" );
	    } else {
		fprintf( stderr, "From: unbalanced )\n" );
	    }
	    return( 1 );
	}

	if ( next_c != NULL ) {
	    if (( result = header_mbox_correct( next_l, next_c )) != 0 ) {
		return( result );
	    }

	    return( 0 );

	} else {
	    /* XXX blank line.  correct? */
	    fprintf( stderr, "From: no data\n" );
	    return( 1 );
	}
    }

    if (( len = ( strlen( simta_headers[ HEAD_FROM ].h_key ) +
	    strlen( sender ) + 3 )) > prepend_len ) {
	if (( prepend_line = (char*)realloc( prepend_line, len ))
		== NULL ) {
	    perror( "realloc" );
	    return( -1 );
	}

	prepend_len = len;
    }

    sprintf( prepend_line, "%s: %s",
	    simta_headers[ HEAD_FROM ].h_key, sender );

    if (( simta_headers[ HEAD_FROM ].h_line =
	    line_prepend( lf, prepend_line )) == NULL ) {
	perror( "malloc" );
	return( -1 );
    }

    /* XXX Sender */

    return( 0 );
}


    int
line_token_qs( struct line_token *token, struct line *l, char *start )
{
    if ( *start != '"' ) {
	return( 1 );
    }

    token->t_start = start;
    token->t_start_line = l;
    token->t_type = TOKEN_QUOTED_STRING;

    for ( ; ; ) {
	start++;

	switch( *start ) {

	case '"':
	    /* end of quoted string */
	    token->t_end = start;
	    token->t_end_line = l;
	    return( 0 );

	case '\\':
	    start++;

	    if ( *start != '\0' ) {
		/* XXX should a trailing '\' be illegal? */
	    	break;
	    }

	case '\0':
	    /* end of line.  if next line starts with WSP, continue */
	    l = l->line_next;

	    if (( l != NULL ) &&
		    ((( *(l->line_data)) == ' ' ) ||
		    (( *(l->line_data)) == '\t' ))) {
		start = l->line_data;
		break;

	    } else {
		/* End of header, no matching '"' */
		return( 1 );
	    }

	default:
	    /* everything else */
	    break;

	}
    }
}


    int
line_token_dl( struct line_token *token, struct line *l, char *start )
{
    if ( *start != '[' ) {
	return( 1 );
    }

    token->t_start = start;
    token->t_start_line = l;
    token->t_type = TOKEN_DOMAIN_LITERAL;

    for ( ; ; ) {
	start++;

	switch( *start ) {

	case ']':
	    /* end of domain literal */
	    token->t_end = start;
	    token->t_end_line = l;
	    return( 0 );

	case '\\':
	    start++;

	    if ( *start != '\0' ) {
		/* XXX should a trailing '\' be illegal? */
	    	break;
	    }

	case '\0':
	    /* end of line.  if next line starts with WSP, continue */
	    l = l->line_next;

	    if (( l != NULL ) &&
		    ((( *(l->line_data)) == ' ' ) ||
		    (( *(l->line_data)) == '\t' ))) {
		start = l->line_data;
		break;

	    } else {
		/* End of header, no matching ']' */
		return( 1 );
	    }

	default:
	    /* everything else */
	    break;

	}
    }
}


    int
is_dot_atom_text( int c )
{
    if ( isalpha( c ) != 0 ) {
	return( 1 );
    }

    if ( isdigit( c ) != 0 ) {
	return( 1 );
    }

    switch ( c ) {

    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '/':
    case '=':
    case '?':
    case '^':
    case '_':
    case '`':
    case '{':
    case '|':
    case '}':
    case '~':
    case '.':
	return( 1 );

    default:
	return( 0 );
    }
}


    int
line_token_da( struct line_token *token, struct line *l, char *start )
{
    token->t_start = start;
    token->t_start_line = l;
    token->t_end_line = l;
    token->t_type = TOKEN_DOT_ATOM;

    if ( is_dot_atom_text( *start ) == 0 ) {
	return( 1 );
    }

    for ( ; ; ) {
	if ( is_dot_atom_text( *(start + 1)) == 0 ) {
	    token->t_end = start;
	    return( 0 );
	}

	start++;
    }
}
