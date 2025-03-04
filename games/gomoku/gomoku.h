/*	$NetBSD: gomoku.h,v 1.31 2022/05/20 19:30:17 rillig Exp $	*/

/*
 * Copyright (c) 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Ralph Campbell.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)gomoku.h	8.2 (Berkeley) 5/3/95
 */

#include <sys/types.h>
#include <sys/endian.h>
#include <stdbool.h>
#include <stdio.h>

/* board dimensions */
#define BSZ	19
#define BAREA	((1 + BSZ + 1) * (BSZ + 1) + 1)

#define TRANSCRIPT_COL	(3 + (2 * BSZ - 1) + 3 + 3)

/* frame dimensions (based on 5 in a row) */
#define FAREA	(2 * BSZ * (BSZ - 4) + 2 * (BSZ - 4) * (BSZ - 4))

#define MUP	(BSZ + 1)
#define MDOWN	(-(BSZ + 1))
#define MLEFT	(-1)
#define MRIGHT	(1)

/* values for s_occ */
#define BLACK	0
#define WHITE	1
#define EMPTY	2
#define BORDER	3

/* return values for makemove() */
#define MOVEOK	0
#define RESIGN	1
#define ILLEGAL	2
#define WIN	3
#define TIE	4
#define SAVE	5

#define PT(x, y)	((x) + (BSZ + 1) * (y))

/*
 * A 'frame' is a group of five or six contiguous board locations.
 * An open-ended frame is one with spaces on both ends; otherwise, its closed.
 * A 'combo' is a group of intersecting frames and consists of two numbers:
 * 'A' is the number of moves to make the combo non-blockable.
 * 'B' is the minimum number of moves needed to win once it can't be blocked.
 * A 'force' is a combo that is one move away from being non-blockable
 *
 * Single frame combo values:
 *     <A,B>	board values
 *	5,0	. . . . . O
 *	4,1	. . . . . .
 *	4,0	. . . . X O
 *	3,1	. . . . X .
 *	3,0	. . . X X O
 *	2,1	. . . X X .
 *	2,0	. . X X X O
 *	1,1	. . X X X .
 *	1,0	. X X X X O
 *	0,1	. X X X X .
 *	0,0	X X X X X O
 *
 * The rule for combining two combos (<A1,B1> <A2,B2>)
 * with V valid intersection points, is:
 *	A' = A1 + A2 - 2 - V
 *	B' = MIN(A1 + B1 - 1, A2 + B2 - 1)
 * Each time a frame is added to the combo, the number of moves to complete
 * the force is the number of moves needed to 'fill' the frame plus one at
 * the intersection point. The number of moves to win is the number of moves
 * to complete the best frame minus the last move to complete the force.
 * Note that it doesn't make sense to combine a <1,x> with anything since
 * it is already a force. Also, the frames have to be independent so a
 * single move doesn't affect more than one frame making up the combo.
 *
 * Rules for comparing which of two combos (<A1,B1> <A2,B2>) is better:
 * Both the same color:
 *	<A',B'> = (A1 < A2 || A1 == A2 && B1 <= B2) ? <A1,B1> : <A2,B2>
 *	We want to complete the force first, then the combo with the
 *	fewest moves to win.
 * Different colors, <A1,B1> is the combo for the player with the next move:
 *	<A',B'> = A2 <= 1 && (A1 > 1 || A2 + B2 < A1 + B1) ? <A2,B2> : <A1,B1>
 *	We want to block only if we have to (i.e., if they are one move away
 *	from completing a force, and we don't have a force that we can
 *	complete which takes fewer or the same number of moves to win).
 */

#define MAXCOMBO	0x600

union comboval {
	struct {
#if BYTE_ORDER == BIG_ENDIAN
		u_char	a;	/* # moves to complete force */
		u_char	b;	/* # moves to win */
#endif
#if BYTE_ORDER == LITTLE_ENDIAN
		u_char	b;	/* # moves to win */
		u_char	a;	/* # moves to complete force */
#endif
	} c;
	u_short	s;
};

/*
 * This structure is used to record information about single frames (F) and
 * combinations of two more frames (C).
 * For combinations of two or more frames, there is an additional
 * array of pointers to the frames of the combination which is sorted
 * by the index into the frames[] array. This is used to prevent duplication
 * since frame A combined with B is the same as B with A.
 *	struct combostr *c_sort[size c_nframes];
 * The leaves of the tree (frames) are numbered 0 (bottom, leftmost)
 * to c_nframes - 1 (top, right). This is stored in c_frameindex and
 * c_dir if C_LOOP is set.
 */
struct combostr {
	struct combostr	*c_next;	/* list of combos at the same level */
	struct combostr	*c_prev;	/* list of combos at the same level */
	struct combostr	*c_link[2];	/* C:previous level or F:NULL */
	union comboval	c_linkv[2];	/* C:combo value for link[0,1] */
	union comboval	c_combo;	/* C:combo value for this level */
	u_short		c_vertex;	/* C:intersection or F:frame head */
	u_char		c_nframes;	/* number of frames in the combo */
	u_char		c_dir;		/* C:loop frame or F:frame direction */
	u_char		c_flags;	/* C:combo flags */
	u_char		c_frameindex;	/* C:intersection frame index */
	u_char		c_framecnt[2];	/* number of frames left to attach */
	u_char		c_emask[2];	/* C:bit mask of completion spots for
					 * link[0] and link[1] */
	u_char		c_voff[2];	/* C:vertex offset within frame */
};

/* flag values for c_flags */
#define C_OPEN_0	0x01		/* link[0] is an open-ended frame */
#define C_OPEN_1	0x02		/* link[1] is an open-ended frame */
#define C_LOOP		0x04		/* link[1] intersects previous frame */

/*
 * This structure is used for recording the completion points of
 * multi frame combos.
 */
struct	elist {
	struct elist	*e_next;	/* list of completion points */
	struct combostr	*e_combo;	/* the whole combo */
	u_char		e_off;		/* offset in frame of this empty spot */
	u_char		e_frameindex;	/* intersection frame index */
	u_char		e_framecnt;	/* number of frames left to attach */
	u_char		e_emask;	/* real value of the frame's emask */
	union comboval	e_fval;		/* frame combo value */
};

/*
 * One spot structure for each location on the board.
 * A frame consists of the combination for the current spot plus the five spots
 * 0: right, 1: right & down, 2: down, 3: down & left.
 */
struct	spotstr {
	short		s_occ;		/* color of occupant */
	short		s_wval;		/* weighted value */
	int		s_flags;	/* flags for graph walks */
	struct combostr	*s_frame[4];	/* level 1 combo for frame[dir] */
	union comboval	s_fval[2][4];	/* combo value for [color][frame] */
	union comboval	s_combo[2];	/* minimum combo value for BLK & WHT */
	u_char		s_level[2];	/* number of frames in the min combo */
	u_char		s_nforce[2];	/* number of <1,x> combos */
	struct elist	*s_empty;	/* level n combo completion spots */
	struct elist	*s_nempty;	/* level n+1 combo completion spots */
	int		dummy[2];	/* XXX */
};

/* flag values for s_flags */
#define CFLAG		0x000001	/* frame is part of a combo */
#define CFLAGALL	0x00000F	/* all frame directions marked */
#define IFLAG		0x000010	/* legal intersection point */
#define IFLAGALL	0x0000F0	/* any intersection points? */
#define FFLAG		0x000100	/* frame is part of a <1,x> combo */
#define FFLAGALL	0x000F00	/* all force frames */
#define MFLAG		0x001000	/* frame has already been seen */
#define MFLAGALL	0x00F000	/* all frames seen */
#define BFLAG		0x010000	/* frame intersects border or dead */
#define BFLAGALL	0x0F0000	/* all frames dead */

extern	const char	*letters;
extern	const char	pdir[];

extern	const int     dd[4];
extern	struct	spotstr	board[BAREA];		/* info for board */
extern	struct	combostr frames[FAREA];		/* storage for single frames */
extern	struct	combostr *sortframes[2];	/* sorted, non-empty frames */
extern	u_char	overlap[FAREA * FAREA];		/* frame [a][b] overlap */
extern	short	intersect[FAREA * FAREA];	/* frame [a][b] intersection */
extern	int	movelog[BSZ * BSZ];		/* history of moves */
extern	int	movenum;
extern	int	debug;

extern bool interactive;
extern const char *plyr[];

void	bdinit(struct spotstr *);
int	get_coord(void);
int	get_key(const char *);
bool	get_line(char *, int);
void	ask(const char *);
void	dislog(const char *);
void	bdump(FILE *);
void	bdisp(void);
void	bdisp_init(void);
void	cursfini(void);
void	cursinit(void);
void	bdwho(bool);
void	panic(const char *, ...) __printflike(1, 2) __dead;
void	debuglog(const char *, ...) __printflike(1, 2);
void	whatsup(int);
const char   *stoc(int);
int	ctos(const char *);
int	makemove(int, int);
void	clearcombo(struct combostr *, int);
void	markcombo(struct combostr *);
int	pickmove(int);
#if defined(DEBUG)
void	printcombo(struct combostr *, char *, size_t);
#endif
