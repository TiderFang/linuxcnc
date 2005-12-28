/********************************************************************
* Description: tc.c
*   Discriminate-based trajectory planning
*
*   Derived from a work by Fred Proctor & Will Shackleford
*
* Author:
* License: GPL Version 2
* System: Linux
*    
* Copyright (c) 2004 All rights reserved.
*
* Last change:
* $Revision$
* $Author$
* $Date$
********************************************************************/

#ifdef ULAPI
#include <stdio.h>
#endif
/*
  FIXME-- should include <stdlib.h> for sizeof(), but conflicts with
  a bunch of <linux> headers
  */
#include "rtapi.h"		/* rtapi_print_msg */
#include "posemath.h"
#include "emcpos.h"
#include "tc.h"

// FIXME - debug only, remove later
#include <hal.h>
#include "../motion/motion.h"
#include "../motion/mot_priv.h"

// FIXME - debug only, remove later
extern int print;


#define TC_VEL_EPSILON 0.0001	/* number below which v is considered 0 */
#define TC_SCALE_EPSILON 0.0001	/* number below which scale is considered 0 */

int tcInit(TC_STRUCT * tc)
{
    PmPose zero;

    if (0 == tc) {
	return -1;
    }

    tc->cycleTime = 0.0;
    tc->targetPos = 0.0;
    tc->vMax = 0.0;
    tc->ini_maxvel = 0.0;
    tc->vScale = 1.0;
    tc->aMax = 0.0;
    tc->preVMax = 0.0;
    tc->preAMax = 0.0;
    tc->vLimit = 0.0;
    tc->toGo = 0.0;
    tc->currentPos = 0.0;
    tc->currentVel = 0.0;
    tc->currentAccel = 0.0;
    tc->tcFlag = TC_IS_UNSET;
    tc->type = TC_LINEAR;	/* default is linear interpolation */
    tc->id = 0;
    tc->termCond = TC_TERM_COND_BLEND;

    tc->tmag = 0.0;
    tc->abc_mag = 0.0;
    tc->tvMax = 0.0;
    tc->taMax = 0.0;
    tc->abc_vMax = 0.0;
    tc->abc_aMax = 0.0;
    tc->unitCart.x = tc->unitCart.y = tc->unitCart.z = 0.0;
    
    // FIXME - debug only, remove later
    tc->output_chan = 0;
    
    zero.tran.x = zero.tran.y = zero.tran.z = 0.0;
    zero.rot.s = 1.0;
    zero.rot.x = zero.rot.y = zero.rot.z = 0.0;

    pmLineInit(&tc->line, zero, zero);
    pmLineInit(&tc->line_abc, zero, zero);
    return 0;
}

int tcSetLine(TC_STRUCT * tc, PmLine line, PmLine line_abc)
{
    double tmag, abc_mag;

    if (0 == tc) {
	return -1;
    }

    tc->line = line;
    tc->line_abc = line_abc;

    /* set targetPos to be scalar difference */
    pmCartCartDisp(line.end.tran, line.start.tran, &tmag);
    pmCartCartDisp(line_abc.end.tran, line_abc.start.tran, &abc_mag);

    tc->abc_mag = abc_mag;
    tc->tmag = tmag;

    if (tc->abc_aMax <= 0.0 && tc->taMax > 0.0) {
	tc->abc_aMax = tc->taMax;
    }

    if (tc->abc_vMax <= 0.0 && tc->tvMax > 0.0) {
	tc->abc_vMax = tc->tvMax;
    }

    if (tc->tmag < 1e-6) {
	tc->aMax = tc->abc_aMax;
	tc->vMax = tc->abc_vMax;
	tc->targetPos = abc_mag;
    } else {
        tc->aMax = tc->taMax;
        tc->vMax = tc->tvMax;
	tc->targetPos = tmag;
    }

    tc->currentPos = 0.0;
    tc->type = TC_LINEAR;
    return 0;
}

int tcSetCircle(TC_STRUCT * tc, PmCircle circle, PmLine line_abc)
{
    double zc;

    if (0 == tc) {
	return -1;
    }

    tc->circle = circle;
    tc->line_abc = line_abc;

    /* get the z component of the helix (this is not necessarily the 
       machine's z - this is z of the helix's cylindrical coordinate
       system) */
    pmCartMag(circle.rHelix, &zc);

    /* for circular/helical motion, path param is the helical length */
    tc->tmag = tc->targetPos =
	pmSqrt(pmSq(circle.angle * circle.radius) + pmSq(zc));

    tc->currentPos = 0.0;
    tc->type = TC_CIRCULAR;

    tc->aMax = tc->taMax;
    tc->vMax = tc->tvMax;

    pmCartCartDisp(line_abc.end.tran, line_abc.start.tran, &tc->abc_mag);

    return 0;
}

int tcRunCycle(TC_STRUCT * tc)
{
    double newPos;
    double newVel;
    double newAccel;
    double discr;
    int isScaleDecel;
    int oldTcFlag;

    
    if (0 == tc) {
    
	// FIXME - debug only, remove later
	rtapi_print("ERROR: tcRunCycle called with tc = NULL\n" );
	
	return -1;
    }

    /* save the old flag-- we'll use it when deciding to flag deceleration if 
       we're scaling back velocity */
    oldTcFlag = tc->tcFlag;

    if (tc->tcFlag == TC_IS_DONE) {
	tc->currentVel = 0.0;
	tc->currentAccel = 0.0;
	
	// FIXME - debug only, remove later
	rtapi_print("tcRunCycle: flag = TC_IS_DONE\n" );
	
	return 0;
    }

    tc->toGo = tc->targetPos - tc->currentPos;

    if (tc->aMax <= 0.0 || tc->vMax <= 0.0 || tc->cycleTime <= 0.0) {
    
	// FIXME - debug only, remove later
	rtapi_print("ERROR: tcRunCycle called with aMax, vMax, or cycleTime <= 0.0\n" );
	
	return -1;
    }

    /* compute newvel = 0 limit first */
    discr = 0.5 * tc->cycleTime * tc->currentVel - tc->toGo;
    if (discr > 0.0) {
	// need to stop asap
	newVel = 0.0;
    } else {
	discr =
	    0.25 * tc->cycleTime * tc->cycleTime - 2.0 / tc->aMax * discr;
	newVel = -0.5 * tc->aMax * tc->cycleTime + tc->aMax * sqrt(discr);
    }
    if (newVel <= 0.0) {
	newVel = 0.0;
	newAccel = 0;
	newPos = tc->targetPos;
	tc->tcFlag = TC_IS_DONE;
    } else {
	/* clamp velocity to scaled max, and note if it's scaled back. This
	   will cause a deceleration which we will NOT flag as a TC_IS_DECEL
	   unless we were previously in a decel mode, since that would cause
	   the next motion in the queue to begin to be planned. Make it
	   TC_IS_CONST instead. */
	isScaleDecel = 0;
	if (newVel > (tc->vMax - tc->preVMax) * tc->vScale) {
	    newVel = (tc->vMax - tc->preVMax) * tc->vScale;
	    isScaleDecel = 1;
	}

	/* clamp scaled velocity against absolute limit */
	if (newVel > tc->vLimit) {
	    newVel = tc->vLimit;
	}

	if (newVel > tc->ini_maxvel) {
	    newVel = tc->ini_maxvel;
	}

	if (tc->type == TC_CIRCULAR) {
	    if (newVel > pmSqrt(tc->aMax * tc->circle.radius)) {
		newVel = pmSqrt(tc->aMax * tc->circle.radius);
	    }
	}

	/* calc resulting accel */
	newAccel = (newVel - tc->currentVel) / tc->cycleTime;

	/* clamp accel if necessary, and recalc velocity */
	/* also give credit for previous segment's decel, in preAMax, which
	   is a negative value since it's a decel */
	if (newAccel > 0.0) {
	    if (newAccel > tc->aMax - tc->preAMax) {
		newAccel = tc->aMax - tc->preAMax;
		/* if tc->preMax was calculated correctly this check is
		   redundant. (Just because I'm paranoid doesn't mean they
		   are not out to get me!) */
		if (newAccel < 0.0) {
		    newAccel = 0.0;
		}
		newVel = tc->currentVel + newAccel * tc->cycleTime;
	    }
	} else {
	    if (newAccel < -tc->aMax) {
		newAccel = -tc->aMax;
		newVel = tc->currentVel + newAccel * tc->cycleTime;
	    }
	}

#ifdef A_CHANGE_MAX
	if (newAccel > A_CHANGE_MAX * tc->cycleTime + tc->currentAccel) {
	    newAccel = A_CHANGE_MAX * tc->cycleTime + tc->currentAccel;
	    newVel = tc->currentVel + newAccel * tc->cycleTime;
	} else if (newAccel <
		   -A_CHANGE_MAX * tc->cycleTime + tc->currentAccel) {
	    newAccel = -A_CHANGE_MAX * tc->cycleTime + tc->currentAccel;
	    newVel = tc->currentVel + newAccel * tc->cycleTime;
	}
#endif

	tc->toGo = (newVel + tc->currentVel) * 0.5 * tc->cycleTime;
	newPos = tc->currentPos + tc->toGo;

	if (newAccel < 0.0) {
	    if (isScaleDecel && oldTcFlag != TC_IS_DECEL) {
		/* we're decelerating, but blending next move is not being
		   done yet, so don't flag a decel. This will prevent
		   premature blending */
		tc->tcFlag = TC_IS_ACCEL;
	    } else {
		tc->tcFlag = TC_IS_DECEL;
	    }
	} else if (newAccel > 0.0) {
	    tc->tcFlag = TC_IS_ACCEL;
	} else if (newVel < TC_VEL_EPSILON
		   && tc->vScale < TC_SCALE_EPSILON) {
	    tc->tcFlag = TC_IS_PAUSED;
	} else {
	    tc->tcFlag = TC_IS_CONST;
	}
    }

    tc->currentPos = newPos;
    tc->currentVel = newVel;
    tc->currentAccel = newAccel;
    
    // FIXME - debug only, remove later
    emcmot_hal_data->tc_pos[tc->output_chan] = newPos;
    emcmot_hal_data->tc_vel[tc->output_chan] = newVel;
    emcmot_hal_data->tc_acc[tc->output_chan] = newAccel;
    
    return 0;
}

EmcPose tcGetPos(TC_STRUCT * tc)
{
    EmcPose v;
    PmPose v1;
    PmPose v2;

    if (0 == tc) {
	v1.tran.x = v1.tran.y = v1.tran.z = 0.0;
	v1.rot.s = 1.0;
	v1.rot.x = v1.rot.y = v1.rot.z = 0.0;
	return v;
    }

    /* note: was if tc->targetPos <= 0.0 return basePos */
    if (tc->type == TC_LINEAR) {
	pmLinePoint(&tc->line, tc->currentPos, &v1);
    } else if (tc->type == TC_CIRCULAR) {
	pmCirclePoint(&tc->circle,
		      tc->currentPos * tc->circle.angle / tc->targetPos,
		      &v1);
    } else {
	v1.tran.x = v1.tran.y = v1.tran.z = 0.0;
	v1.rot.s = 1.0;
	v1.rot.x = v1.rot.y = v1.rot.z = 0.0;
    }
    v.tran = v1.tran;
    if (tc->abc_mag > 1e-6) {
	if (tc->tmag > 1e-6) {
	    pmLinePoint(&tc->line_abc,
			(tc->currentPos * tc->abc_mag / tc->tmag), &v2);
	    v.a = v2.tran.x;
	    v.b = v2.tran.y;
	    v.c = v2.tran.z;
	} else {
	    pmLinePoint(&tc->line_abc, tc->currentPos, &v2);
	    v.a = v2.tran.x;
	    v.b = v2.tran.y;
	    v.c = v2.tran.z;
	}
    } else {
	v.a = v.b = v.c = 0.0;
    }

    return v;
}

int tcIsPaused(TC_STRUCT * tc)
{
    if (0 == tc) {
	return 0;
    }

    return (tc->tcFlag == TC_IS_PAUSED);
}

void tcPrint(TC_STRUCT * tc)
{
#ifdef ULAPI
    /* We don't really want to print this lot out from within a realtime
       module as it will send kernel logging daemon nuts */

    if (0 == tc) {
	rtapi_print_msg(1, "\n");
	return;
    }

    rtapi_print_msg(1, "cycleTime:    %f\n", tc->cycleTime);
    rtapi_print_msg(1, "targetPos:    %f\n", tc->targetPos);
    rtapi_print_msg(1, "vMax:         %f\n", tc->vMax);
    rtapi_print_msg(1, "vLimit:       %f\n", tc->vLimit);
    rtapi_print_msg(1, "vScale        %f\n", tc->vScale);
    rtapi_print_msg(1, "aMax:         %f\n", tc->aMax);
    rtapi_print_msg(1, "toGo:         %f\n", tc->toGo);
    rtapi_print_msg(1, "currentPos:   %f\n", tc->currentPos);
    rtapi_print_msg(1, "currentVel:   %f\n", tc->currentVel);
    rtapi_print_msg(1, "currentAccel: %f\n", tc->currentAccel);
    rtapi_print_msg(1, "tcFlag:       %s\n",
		    tc->tcFlag == TC_IS_UNSET ? "UNSET" : tc->tcFlag ==
		    TC_IS_DONE ? "DONE" : tc->tcFlag ==
		    TC_IS_ACCEL ? "ACCEL" : tc->tcFlag ==
		    TC_IS_CONST ? "CONST" : tc->tcFlag ==
		    TC_IS_DECEL ? "DECEL" : tc->tcFlag ==
		    TC_IS_PAUSED ? "PAUSED" : "?");
    rtapi_print_msg(1, "type:         %s\n",
		    tc->type == TC_LINEAR ? "LINEAR" : tc->type ==
		    TC_CIRCULAR ? "CIRCULAR" : "?");
    rtapi_print_msg(1, "id:           %d\n", tc->id);
#endif				/* ULAPI */
}

/* TC_QUEUE_STRUCT definitions */

int tcqCreate(TC_QUEUE_STRUCT * tcq, int _size, TC_STRUCT * tcSpace)
{
    if (_size <= 0 || 0 == tcq) {
	return -1;
    } else {
	tcq->queue = tcSpace;
	tcq->size = _size;
	tcq->_len = 0;
	tcq->start = tcq->end = 0;
	tcq->allFull = 0;

	if (0 == tcq->queue) {
	    return -1;
	}
	return 0;
    }
}

int tcqDelete(TC_QUEUE_STRUCT * tcq)
{
    if (0 != tcq && 0 != tcq->queue) {
	/* free(tcq->queue); */
	tcq->queue = 0;
    }

    return 0;
}

int tcqInit(TC_QUEUE_STRUCT * tcq)
{
    if (0 == tcq) {
	return -1;
    }

    tcq->_len = 0;
    tcq->start = tcq->end = 0;
    tcq->allFull = 0;

    return 0;
}

/* put sometc on the end of the queue */
int tcqPut(TC_QUEUE_STRUCT * tcq, TC_STRUCT tc)
{

    /* check for initialized */
    if (0 == tcq || 0 == tcq->queue) {
	return -1;
    }

    /* check for allFull, so we don't overflow the queue */
    if (tcq->allFull) {
	return -1;
    }

    /* add it */
    tcq->queue[tcq->end] = tc;
    tcq->_len++;

    /* update end ptr, modulo size of queue */
    tcq->end = (tcq->end + 1) % tcq->size;

    /* set allFull flag if we're really full */
    if (tcq->end == tcq->start) {
	tcq->allFull = 1;
    }

    return 0;
}

/* remove n items from the queue */
int tcqRemove(TC_QUEUE_STRUCT * tcq, int n)
{

    if (n <= 0) {
	return 0;		/* okay to remove 0 or fewer */
    }

    if ((0 == tcq) || (0 == tcq->queue) ||	/* not initialized */
	((tcq->start == tcq->end) && !tcq->allFull) ||	/* empty queue */
	(n > tcq->_len)) {	/* too many requested */
	return -1;
    }

    /* update start ptr and reset allFull flag and len */
    tcq->start = (tcq->start + n) % tcq->size;
    tcq->allFull = 0;
    tcq->_len -= n;

    return 0;
}

/* how many items are in the queue? */
int tcqLen(TC_QUEUE_STRUCT * tcq)
{
    if (0 == tcq) {
	return -1;
    }

    return tcq->_len;
}

/* get the nth item from the queue, [0..len-1], without removing */
TC_STRUCT *tcqItem(TC_QUEUE_STRUCT * tcq, int n, int *status)
{
    if ((0 == tcq) || (0 == tcq->queue) ||	/* not initialized */
	(n < 0) || (n >= tcq->_len)) {	/* n too large */
	if (0 != status) {
	    *status = -1;
	}
	return (TC_STRUCT *) 0;
    }

    if (0 != status) {
	*status = 0;
    }
    return &(tcq->queue[(tcq->start + n) % tcq->size]);
}

/* get the full status of the queue */
#define TC_QUEUE_MARGIN 10
int tcqFull(TC_QUEUE_STRUCT * tcq)
{
    if (0 == tcq) {
	return 1;		/* null queue is full, for safety */
    }

    /* call the queue full if the length is into the margin, so reduce the
       effect of a race condition where the appending process may not see the 
       full status immediately and send another motion */

    if (tcq->size <= TC_QUEUE_MARGIN) {
	/* no margin available, so full means really all full */
	return tcq->allFull;
    }

    if (tcq->_len >= tcq->size - TC_QUEUE_MARGIN) {
	/* we're into the margin, so call it full */
	return 1;
    }

    /* we're not into the margin */
    return 0;
}


PmCartesian tcGetUnitCart(TC_STRUCT * tc)
{
    PmPose currentPose;
    PmCartesian radialCart;
    static const PmCartesian fake = { 1.0, 0.0, 0.0 };

    if (tc->type == TC_LINEAR) {
	pmCartCartSub(tc->line.end.tran, tc->line.start.tran,
		      &tc->unitCart);
#ifdef USE_PM_CART_NORM
	pmCartNorm(tc->unitCart, &tc->unitCart);
#else
	pmCartUnit(tc->unitCart, &tc->unitCart);
#endif
	return (tc->unitCart);
    } else if (tc->type == TC_CIRCULAR) {
	pmCirclePoint(&tc->circle, tc->currentPos, &currentPose);
	pmCartCartSub(currentPose.tran, tc->circle.center, &radialCart);
	pmCartCartCross(tc->circle.normal, radialCart, &tc->unitCart);
#ifdef USE_PM_CART_NORM
	pmCartNorm(tc->unitCart, &tc->unitCart);
#else
	pmCartUnit(tc->unitCart, &tc->unitCart);
#endif
	return (tc->unitCart);
    }
    // It should never really get here.
    return fake;
}

