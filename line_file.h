/*
 * Copyright (c) 1998 Regents of The University of Michigan.
 * All Rights Reserved.  See COPYRIGHT.
 */

/*****     line_file.h     *****/


struct line {
    struct line		*line_next;
    struct line		*line_prev;
    char		*line_data;
};

struct line_file {
    struct line		*l_first;
    struct line		*l_last;
};

struct header {
    char                *h_key;
    struct line         *h_line;
    char                *h_data;
};

#ifdef __STDC__
#define ___P(x)         x
#else /* __STDC__ */
#define ___P(x)         ()
#endif /* __STDC__ */

/* public */
struct line_file	*line_file_create ___P(( void ));
void		line_file_free ___P(( struct line_file * ));
struct line	*line_append ___P(( struct line_file *, char * ));
struct line	*line_prepend ___P(( struct line_file *, char * ));

/* private */
void		line_free ___P(( struct line * ));
