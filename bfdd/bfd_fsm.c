/*
 * BFDD - bfd_fsm.c
 *
 * Copyright (C) 2007   Jaroslaw Adam Gralak
 *
 * This program is free software; you can redistribute it and/or modify it 
 * under the terms of the GNU General Public Licenseas published by the Free 
 * Software Foundation; either version 2 of the License, or (at your option) 
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,but WITHOUT 
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for 
 * more details.

 * You should have received a copy of the GNU General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */


#include <zebra.h>

#include "log.h"
#include "thread.h"
#include "sockunion.h"

#include "bfdd/bfdd.h"
#include "bfdd/bfd_fsm.h"
#include "bfdd/bfd_debug.h"
#include "bfdd/bfd_interface.h"
#include "bfdd/bfd_packet.h"
#include "bfdd/bfd_zebra.h"

extern struct message bfd_status_msg[];
extern int bfd_status_msg_max;
extern struct thread_master *master;

/* BFD FSM timer aka liveness detection timer, it's responsibility
   is to verify if our link partner is still alive */
int
bfd_fsm_timer (struct thread *thread)
{
  struct bfd_neigh *neighp;

  neighp = THREAD_ARG (thread);
  neighp->t_timer = NULL;

  if (BFD_IF_DEBUG_FSM)
    BFD_FSM_LOG_DEBUG_NOARG ("Timer expired")
      /* If session transitioned from Up state because of "TIMER"
         set corresponding diagnostic message */
      if (neighp->status == FSM_S_Up)
      {
	neighp->ldiag = BFD_DIAG_TIMEEXPIRED;
	neighp->timer_cnt++;
      }

  bfd_event (neighp, FSM_E_Timer);

  return BFD_OK;
}

/* Session timeout timer, activated each time
   when session goes into Down state. */
int
bfd_fsm_stimeout (struct thread *thread)
{
  struct bfd_neigh *neighp;

  neighp = THREAD_ARG (thread);
  neighp->t_session = NULL;

  /* Check if session state is down and if it was any
     activity since timer has been fired */
  if ((neighp->status == FSM_S_Down)
      && (neighp->orecv_cnt == neighp->recv_cnt))
    {
      /* There wasn't any activity our link neighbor 
         is most probably dead or was administratively disabled */
      if (BFD_IF_DEBUG_FSM)
	BFD_FSM_LOG_DEBUG_NOARG ("Session timeout.")
	  /* Reset "Your discriminator" */
	  neighp->rdisc = 0;

      /* Reset diagnostic */
      neighp->ldiag = 0;

      /* Reset timers to default values */
      neighp->ldesmintx = BFD_LDESMINTX_DFT;
      neighp->ldesmintx_a = BFD_LDESMINTX_DFT;
      neighp->rreqminrx = BFD_RREQMINRX_DFT;
      neighp->txint = BFD_LDESMINTX_DFT;

      neighp->lreqminrx = BFD_LREQMINRX_DFT;
      neighp->lreqminrx_a = BFD_LREQMINRX_DFT;
      neighp->rdesmintx = BFD_RREQMINRX_DFT;

      neighp->lreqminechorx = BFD_REQMINECHORX_DFT;
      neighp->rreqminechorx = BFD_REQMINECHORX_DFT;

      neighp->lmulti = BFD_DFT_MULTI;
      neighp->rmulti = BFD_DFT_MULTI;

      /* Clear flags (bits) */
      neighp->lbits = 0;
      neighp->rbits = 0;

      neighp->notify = 0;

      /* Update passive flag in case interface state has changed */
      bfd_neigh_if_passive_update (neighp);
      /* If passive mode is desired stop transmission of periodic BFDCP */
      if (bfd_neigh_check_lbit_p (neighp))
	BFD_TIMER_OFF (neighp->t_hello);
    }
  return BFD_OK;
}

/* Delete timer
   If neighbor removal is requested by zebra (because 
   of administrative purpose), we have to first signalize "AdminDown" 
   state to our link partner and then we can start removing neighbor 
   locally from our database. Delete timer represents time during which 
   we transmit packets to the link neighbor with "AdminDown" state, 
   once "bfd_fsm_delete" function is executed (after delete period)
   session (neighbor) is removed permanently from database.
 */
static int
bfd_fsm_delete (struct thread *thread)
{
  struct bfd_neigh *neighp;

  neighp = THREAD_ARG (thread);
  neighp->t_delete = NULL;

  if (BFD_IF_DEBUG_FSM)
    BFD_FSM_LOG_DEBUG_NOARG ("Removing neighbor")
      BFD_TIMER_OFF (neighp->t_hello);
  bfd_neigh_del (neighp);
  return BFD_OK;
}

/* Fire hello thread for given neighbor */
int
bfd_fsm_neigh_add (struct bfd_neigh *neighp)
{
  if (BFD_IF_DEBUG_FSM){
  	char lbuf[SU_ADDRSTRLEN];
	char rbuf[SU_ADDRSTRLEN];
    zlog_debug ("[FSM] (%s) Add l:%s, r:%s/ldisc:%d, rdisc:%d", __func__,
		sockunion2str (neighp->su_local, lbuf, SU_ADDRSTRLEN),
		sockunion2str (neighp->su_remote, rbuf, SU_ADDRSTRLEN), neighp->ldisc,
		neighp->rdisc);
  	}
  BFD_TIMER_MSEC_ON (neighp->t_hello, bfd_pkt_xmit, BFD_TXINT (neighp));
  return BFD_OK;
}

int
bfd_fsm_neigh_del (struct bfd_neigh *neighp)
{
  if (BFD_IF_DEBUG_FSM){
  	char lbuf[SU_ADDRSTRLEN];
	char rbuf[SU_ADDRSTRLEN];
    zlog_debug ("[FSM] (%s) l:%s, r:%s/ldisc:%d, rdisc:%d", __func__,
		sockunion2str (neighp->su_local, lbuf, SU_ADDRSTRLEN),
		sockunion2str (neighp->su_remote, rbuf, SU_ADDRSTRLEN), neighp->ldisc,
		neighp->rdisc);
  	}

  /* Set delete flag */
  neighp->del = 1;

  /* Stop timers (session, timer) and schedule delete timer */
  BFD_TIMER_OFF (neighp->t_timer);
  BFD_TIMER_OFF (neighp->t_session);
  BFD_TIMER_MSEC_ON (neighp->t_delete, bfd_fsm_delete,
		     MSEC (neighp->negtxint * neighp->lmulti));

  neighp->uptime = 0;

  /* Change state to "AdminDown" */
  bfd_event (neighp, FSM_E_Delete);

  return BFD_OK;
}

/* Do nothing */
static int
bfd_fsm_ignore (struct bfd_neigh *neighp)
{
  if (BFD_IF_DEBUG_FSM)
    BFD_FSM_LOG_DEBUG_NOARG ("ignoring packet") return BFD_OK;
}

static int
bfd_fsm_discard (struct bfd_neigh *neighp)
{
  if (BFD_IF_DEBUG_FSM)
    BFD_FSM_LOG_DEBUG_NOARG ("discarding packet") return BFD_ERR;
}

/* FSM Init state*/
static int
bfd_fsm_init (struct bfd_neigh *neighp)
{
  if ((neighp->notify != FSM_S_Init) && BFD_IF_DEBUG_FSM)
    {
      BFD_FSM_LOG_DEBUG_NOARG ("Init.") neighp->notify = FSM_S_Init;
    }

  neighp->lstate = BFD_STATE_INIT;

  neighp->uptime = 0;

  return BFD_OK;
}

/* FSM Up state */
static int
bfd_fsm_up (struct bfd_neigh *neighp)
{

  neighp->lstate = BFD_STATE_UP;

  /* Check if session is Up on remote system */
  if (neighp->rstate == BFD_STATE_UP)
    {
      struct bfd_if_info *bii = bfd_ifinfo_get (neighp);

      if (neighp->notify != FSM_S_Up)
	{
	  if (BFD_IF_DEBUG_FSM)
	    BFD_FSM_LOG_DEBUG_NOARG ("Up.") bfd_signal_neigh_up (neighp);
	  neighp->notify = FSM_S_Up;
	}

      /* "If either bfd.DesiredMinTxInterval is changed 
         or bfd.RequiredMinRxInterval is changed, a Poll Sequence 
         MUST be initiated" - check also if we not already transmitting BFD CP
         with a Final (F) bit set */
      if (((neighp->ldesmintx != USEC (bii->interval)) ||
	   (neighp->lreqminrx != USEC (bii->minrx))) &&
	  !bfd_neigh_check_lbit_f (neighp))
	{
	  if (bfd_neigh_check_rbit_f (neighp))
	    {
	      neighp->ldesmintx = neighp->ldesmintx_a;
	      neighp->lreqminrx = neighp->lreqminrx_a;

	      neighp->negtxint =
		neighp->rreqminrx >
		neighp->ldesmintx ? neighp->rreqminrx : neighp->ldesmintx;
	      /* Jitter */
	      if (neighp->rmulti == 1)
		neighp->txint = bfd_jtimer_mult_is1 (neighp->negtxint);
	      else
		neighp->txint = bfd_jtimer_mult_isnot1 (neighp->negtxint);
	    }
	  else
	    {
	      neighp->lbits |= BFD_BIT_P;
	      neighp->ldesmintx_a = USEC (bii->interval);
	      neighp->lreqminrx_a = USEC (bii->minrx);
	    }
	}
      /* Refresh detect multiplier */
      neighp->lmulti = bii->multiplier;

      /* If demand mode is localy desired signalized it to neighbor */
      if (bfd_flag_demand_check (neighp))
	neighp->lbits |= BFD_BIT_D;

      /* If check if neighbor wishes to run bfd is the demand mode */
      if (bfd_neigh_check_rbit_d (neighp))
	{
	  /* If remote side has pulled us, and we want to send a 
	     replay with "final" bit set - allow it. 
	     Otherwise stop sending BFD control packets to neighbor */
	  if (bfd_neigh_check_lbit_f (neighp))
	    {
	      neighp->t_hello = NULL;
	      BFD_TIMER_MSEC_ON (neighp->t_hello, bfd_pkt_xmit,
				 BFD_TXINT (neighp));
	    }
	  else
	    BFD_TIMER_OFF (neighp->t_hello);
	}

      if (!neighp->uptime)
	neighp->uptime = time (NULL);
    }
  return BFD_OK;
}

/* FSM AdminDown state */
static int
bfd_fsm_admdown (struct bfd_neigh *neighp)
{
  if ((neighp->notify != FSM_S_AdminDown) && BFD_IF_DEBUG_FSM)
    {
      BFD_FSM_LOG_DEBUG_NOARG ("AdminDown.") neighp->notify = FSM_S_AdminDown;

      /* Send packet with admin down state immediately */
      BFD_TIMER_OFF (neighp->t_hello);
      BFD_TIMER_MSEC_ON (neighp->t_hello, bfd_pkt_xmit, 0);
    }

  /* If we undergo session removal process (delete flag set)
     change signalised state to "AdminDown" and set appropriate
     diagnostic message. Otherwise (i.e. if "del" not set) we
     received a BFD CP with "AdminDown" state so we have to
     signalize back "Down" state and corresponding diagnostic code. */

  if (neighp->del)
    {
      neighp->lstate = BFD_STATE_ADMINDOWN;
      neighp->ldiag = BFD_DIAG_ADMINDOWN;
    }
  else
    {
      neighp->lstate = BFD_STATE_DOWN;
      neighp->ldiag = BFD_DIAG_SESSIONDOWN;
    }

  neighp->uptime = 0;

  return BFD_OK;
}


/* FSM Down state */
static int
bfd_fsm_down (struct bfd_neigh *neighp)
{

  neighp->lstate = BFD_STATE_DOWN;

  if (!neighp->uptime)
    neighp->uptime = time (NULL);

  /* Initialization of session timeout timer */
  if (!neighp->t_session)
    {
      neighp->orecv_cnt = neighp->recv_cnt;
      BFD_TIMER_MSEC_ON (neighp->t_session, bfd_fsm_stimeout,
			 MSEC (neighp->dtime) + BFD_STIMEOUT);
    }

  /* If state is "Up" and we didn't notify zebra yet 
     send notification about state transition to "Down" */
  //if(neighp->status == FSM_S_Up && (neighp->notify != FSM_S_Down))
  if (neighp->notify != FSM_S_Down)
    {
      BFD_FSM_LOG_DEBUG_NOARG ("Down.") neighp->notify = FSM_S_Down;
      if (neighp->status == FSM_S_Up)
	bfd_signal_neigh_down (neighp);
    }
  return BFD_OK;
}

/* BFD Finite State Machine structure

                                  +--+
                                  |  | UP, ADMIN DOWN, TIMER
                                  |  V
                          DOWN  +------+  INIT
                   +------------|      |------------+
                   |            | DOWN |            |
                   |  +-------->|      |<--------+  |
                   |  |         +------+         |  |
                   |  |                          |  |
                   |  |               ADMIN DOWN,|  |
                   |  |ADMIN DOWN,          DOWN,|  |
                   |  |TIMER                TIMER|  |
                   V  |                          |  V
                 +------+                      +------+
            +----|      |                      |      |----+
        DOWN|    | INIT |--------------------->|  UP  |    |INIT, UP
            +--->|      | INIT, UP             |      |<---+
                 +------+                      +------+
*/
struct
{
  int (*func) ();
  int next_state;
} FSM[FSM_S_MAX - 1][FSM_E_MAX - 1] =
{
  /* AdminDown  */
  {
    {
    bfd_fsm_discard, FSM_S_AdminDown},	/* FSM_E_RecvAdminDown */
    {
    bfd_fsm_discard, FSM_S_AdminDown},	/* FSM_E_RecvDown      */
    {
    bfd_fsm_discard, FSM_S_AdminDown},	/* FSM_E_RecvInit      */
    {
    bfd_fsm_discard, FSM_S_AdminDown},	/* FSM_E_RecvUp        */
    {
    bfd_fsm_discard, FSM_S_AdminDown},	/* FSM_E_Timer         */
    {
    bfd_fsm_discard, FSM_S_AdminDown},	/* FSM_E_Delete        */
  },
    /* Down */
  {
    {
    bfd_fsm_ignore, FSM_S_Down},	/* FSM_E_RecvAdminDown */
    {
    bfd_fsm_init, FSM_S_Init},	/* FSM_E_RecvDown      */
    {
    bfd_fsm_up, FSM_S_Up},	/* FSM_E_RecvInit      */
    {
    bfd_fsm_ignore, FSM_S_Down},	/* FSM_E_RecvUp        */
    {
    bfd_fsm_down, FSM_S_Down},	/* FSM_E_Timer         */
    {
    bfd_fsm_admdown, FSM_S_AdminDown},	/* FSM_E_Delete        */
  },
    /* Init */
  {
    {
    bfd_fsm_admdown, FSM_S_Down},	/* FSM_E_RecvAdminDown */
    {
    bfd_fsm_init, FSM_S_Init},	/* FSM_E_RecvDown      */
    {
    bfd_fsm_up, FSM_S_Up},	/* FSM_E_RecvInit      */
    {
    bfd_fsm_up, FSM_S_Up},	/* FSM_E_RecvUp        */
    {
    bfd_fsm_down, FSM_S_Down},	/* FSM_E_Timer         */
    {
    bfd_fsm_admdown, FSM_S_AdminDown},	/* FSM_E_Delete        */
  },
    /* Up */
  {
    {
    bfd_fsm_admdown, FSM_S_Down},	/* FSM_E_RecvAdminDown */
    {
    bfd_fsm_down, FSM_S_Down},	/* FSM_E_RecvDown      */
    {
    bfd_fsm_up, FSM_S_Up},	/* FSM_E_RecvInit      */
    {
    bfd_fsm_up, FSM_S_Up},	/* FSM_E_RecvUp        */
    {
    bfd_fsm_down, FSM_S_Down},	/* FSM_E_Timer         */
    {
    bfd_fsm_admdown, FSM_S_AdminDown},	/* FSM_E_Delete        */
},};

static const char *bfd_event_str[] = {
  NULL,
  "RecvAdminDown",
  "RecvDown",
  "RecvInit",
  "RecvUp",
  "Timer",
  "Delete",
};

/* Event function, responsible for processing FSM events and
   based on current FSM state run appropriate function */
int
bfd_event (struct bfd_neigh *neighp, int event)
{
  int ret = BFD_ERR;
  int next = FSM[neighp->status - 1][event - 1].next_state;

  if (BFD_IF_DEBUG_FSM && neighp->status != next)
    BFD_FSM_LOG_DEBUG ("%s (%s->%s)", bfd_event_str[event],
		       LOOKUP (bfd_status_msg, neighp->status),
		       LOOKUP (bfd_status_msg, next))
      if (FSM[neighp->status - 1][event - 1].func)
      ret = (*(FSM[neighp->status - 1][event - 1].func)) (neighp);

  if (ret == BFD_OK)
    {
      if (next != neighp->status)
	{
	  /* Remember the previous status */
	  neighp->ostatus = neighp->status;
	  neighp->status = next;
	}
    }
  return ret;
}
