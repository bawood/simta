/*
 * Copyright (c) 1998 Regents of The University of Michigan.
 * All Rights Reserved.  See COPYRIGHT.
 */

/**********	message.c	**********/

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include "message.h"


    /* create & return a message structure */

    struct message *
message_create( void )
{
    struct message	*m;

    if (( m = (struct message*)malloc( sizeof( struct message ))) == NULL ) {
	return( NULL );
    }

    m->m_first = NULL;
    m->m_last = NULL;

    return( m );
}


    /* add a line to the message */

    struct line *
message_line( struct message *m, char *line )
{
    struct line		*l;

    if (( l = (struct line*)malloc( sizeof( struct line ))) == NULL ) {
	return( NULL );
    }

    if (( l->line_data = strdup( line )) == NULL ) {
	return( NULL );
    }

    l->line_next = NULL;

    if ( m->m_first == NULL ) {
	m->m_first = l;
	m->m_last = l;

    } else {
	m->m_last->line_next = l;
	m->m_last = l;
    }

    return( l );
}


    /* prepend a line to the message */

    struct line *
message_prepend_line( struct message *m, char *line )
{
    struct line		*l;

    if (( l = (struct line*)malloc( sizeof( struct line ))) == NULL ) {
	return( NULL );
    }

    if (( l->line_data = strdup( line )) == NULL ) {
	return( NULL );
    }

    l->line_next = m->m_first;

    if ( m->m_first == NULL ) {
	m->m_first = l;
	m->m_last = l;

    } else {
	m->m_first = l;
    }

    return( l );
}


    /* print a message to stdout for debugging purposes */

    void
message_stdout( struct message *m )
{
    struct line		*l;
    int			x = 0;

    for ( l = m->m_first; l != NULL ; l = l->line_next ) {
	x++;
	printf( "%d:\t%s\n", x, l->line_data );
    }
}