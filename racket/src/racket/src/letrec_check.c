/*
  Racket
  Copyright (c) 2004-2013 PLT Design Inc.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301 USA.

*/

#include "schpriv.h"

#ifdef MZ_PRECISE_GC
static void register_traversers(void);
#endif

void scheme_init_letrec_check()
{
#ifdef MZ_PRECISE_GC
    register_traversers();
#endif
}

#define DEBUG_LEVEL 0
#define DEBUG(stmt) if (DEBUG_LEVEL) { stmt; }
#define VERBOSE_DEBUG(stmt) if (DEBUG_LEVEL > 1) { stmt; }
#define MODNAME_DEBUG(stmt) if (0 || (DEBUG_LEVEL > 1)) { stmt; }

#define LET_RHS_EXPR   0x1
#define LET_BODY_EXPR (0x1 << 1)
#define LET_NO_EXPR   (0x1 << 2)

#define FRAME_TYPE_LETREC   0x1
#define FRAME_TYPE_LETSTAR (0x1 << 1)
#define FRAME_TYPE_LET     (0x1 << 2)
#define FRAME_TYPE_CLOSURE (0x1 << 3)

typedef Scheme_Object Wrapped_Lhs;

/* struct where all mutable information is stored during this pass! */
typedef struct Letrec_Check_Frame {
    MZTAG_IF_REQUIRED

    /* whether this is a frame for a letrec, let*, let, or closure */
    int frame_type; 
    
    /* TODO: should this really be an mzshort? */
    /* total number of bindings in this frame */
    int count;

    /* number of deferred expressions that must be processed before
       the let can be finished */
    int waiting;

    /* table of lists of deferred sub expressions for each bound
       variable we count all variables here (not just letrec bound
       variables) because we still need to jump over them */
    Scheme_Object **def;

    /* which sub-expression of a letrec we are in, so that we know
       what to record when we find a reference to a local reference */
    int subexpr;

    /* we also need to track which variables are referenced so that
       letrecs can track what variables appear in bodies and RHS.
       this array is always num_bindings long, and indexed by the
       position of local references */
    int *ref;

    /* we need to track which variables we actually add checks around
       so we can update the flags for those variables */
    int *checked;

    /* so we can fix the flags afterwards */
    Scheme_Let_Header *head;

    Scheme_Object *deferred_with_rhs_ref;
    Scheme_Object *deferred_with_body_ref;
    Scheme_Object *deferred_with_no_ref;

    struct Letrec_Check_Frame *next;
} Letrec_Check_Frame;

/* a deferred expression, these are inserted and completely removed by
   the letrec_check pass */
typedef struct { 
    Scheme_Object so;

    /* the expression that has been deferred */
    Scheme_Closure_Data *expr;

    /* the frame that existed when the expr was deferred */
    Letrec_Check_Frame *frame;

    /* the position of the LHS variable associated with this deferred
       RHS binding sub-expression */
    int position;

    /* the environment; i.e. the states of the variables in enclosing
       letrecs at the point of deferral */
    Scheme_Object *uvars;
    Scheme_Object *pvars;

    /* keeps track of the subexpressions of all frames so they can be
       set back to the correct values upon un-deferral */
    Scheme_Object *subexpr_ls;

} Scheme_Deferred_Expr;

void print_frame(Letrec_Check_Frame *frame)
{
    fflush(stdout);

    printf("frame: [ ");
    while(frame != NULL) {
      printf("[%d %d %d] ", frame->frame_type, frame->count, (int)SCHEME_INT_VAL(frame->subexpr));
        frame = frame->next;
    }
    printf("]");
}

/* initializes a Letrec_Check_Frame */
Letrec_Check_Frame *init_letrec_check_frame(int frame_type, 
                                            mzshort count, 
                                            Letrec_Check_Frame *prev,
                                            Scheme_Let_Header *head)
{
    Letrec_Check_Frame *frame;
    Scheme_Object **def;
    int *ref, *checked, i;

    frame = (Letrec_Check_Frame *)MALLOC_ONE_RT(Letrec_Check_Frame);
#ifdef MZTAG_REQUIRED
    frame->type = scheme_rt_letrec_check_frame;
#endif
    
    frame->frame_type = frame_type;
    
    frame->count = count;
    frame->next = prev;
    frame->waiting = 0;

    frame->head = head;

    /* def will be a table of lists so every entry should be
       initialized to scheme_null */
    def = MALLOC_N(Scheme_Object *, count);
    for(i = 0; i < count; i++) { def[i] = scheme_null; }
    frame->def = def;

    /* the sub-expression of the letrec (if we're in a letrec),
       i.e. the RHS or the body.  this is for tracking where LHS
       variables are referenced */
    if (frame_type == FRAME_TYPE_CLOSURE) {
        frame->subexpr = 2;
    }
    else {
        frame->subexpr = -1;
    }

    /* ref is a table of flags, 0 for unreferenced, 1-3 for referenced
       in the body and/or the RHS */
    ref = MALLOC_N(int, count);
    for(i = count; i--;) { ref[i] = 0; }
    frame->ref = ref;

    /* checked is a table of 0s or 1s, whether or not a LHS variable
       had a check added around it */
    checked = MALLOC_N(int, count);
    for(i = count; i--;) { checked[i] = 0; }
    frame->checked = checked;

    frame->deferred_with_rhs_ref = scheme_false;
    frame->deferred_with_body_ref = scheme_false;
    frame->deferred_with_no_ref = scheme_false;

    if (DEBUG_LEVEL > 1) {
        printf("init_letrec_check_frame: type: %d; ", frame->frame_type);
        print_frame(frame->next);
        printf(" -> ");
        print_frame(frame);
        printf("\n"); 
    }

#ifdef MZTAG_REQUIRED
        if (frame->type != scheme_rt_letrec_check_frame) {
            scheme_signal_error("init_letrec_check_frame: frame is not a frame");
        }
        if ((prev != NULL) && (prev->type != scheme_rt_letrec_check_frame)) {
            scheme_signal_error("init_letrec_check_frame: frame is not a frame");
        }
#endif
        
    return frame;
}

/* returns the frame that is the nearest enclosing let in the
   LET_RHS_EXPR */
Letrec_Check_Frame *get_nearest_rhs(Letrec_Check_Frame *frame) {
    for (; frame != NULL; frame = frame->next) {
        if (frame->subexpr < 0) {
            scheme_signal_error("get_nearest_rhs: subexpr is negative");
        }
        if ((frame->subexpr & LET_RHS_EXPR) &&
            (frame->frame_type & FRAME_TYPE_LETREC))
            { return frame; }
    }
    
    scheme_signal_error("get_nearest_rhs: not in a let RHS");
    return frame; // dead
}

/* returns the frame that was created when pos was created, and
   changes pos to be relative to that frame */
Letrec_Check_Frame *get_relative_frame(int *pos, Letrec_Check_Frame *frame)
{
    if (DEBUG_LEVEL > 1) {
        printf("get_relative_frame\n pos_int: %d\n ", *pos);
        print_frame(frame);
        printf("\n");
    }

    /* we've gone wrong if pos_int is negative or if the frame has
       become NULL because pos should have be a valid LHS variable
       reference */
    if (*pos < 0)
        scheme_signal_error("get_relative_frame: pos is negative");
    if (frame == NULL) {
        scheme_signal_error("get_relative_frame: frame is NULL");
    }

    if (frame->subexpr < 0) {
        scheme_signal_error("get_relative_frame: subexpr is negative");
    }

    /* if we're in the RHS of a let, no bindings for the LHS variables
       have been pushed yet, pos can't possibly be in this frame.  so
       don't do any offsetting and look in the next frame */
    if ((frame->frame_type & FRAME_TYPE_LET) &&
        (frame->subexpr & LET_RHS_EXPR)) {
        return get_relative_frame(pos, frame->next);
    }
    else {
        if (*pos >= frame->count) {
            /* we're not in the right frame yet, so offset pos by the
               number of bindings in this frame */
            (*pos) -= frame->count;
            return get_relative_frame(pos, frame->next);
        }
    }
    
    return frame;
}

/* takes an absolute position and returns whether or not that position
   has a reference of the right type */
int pos_has_ref(int position, Letrec_Check_Frame *frame, int type)
{
    int pos_ref = position;

    if (type & LET_NO_EXPR) {
        return 1;
    }
    else {
        frame = get_relative_frame(&pos_ref, frame);
        return ((frame->ref)[pos_ref] & type);
    }
}

/* adds expr to the deferred bindings of lhs */
void update_frame(Letrec_Check_Frame *outer, Letrec_Check_Frame *inner, 
                  int position, Scheme_Deferred_Expr *clos)
{
    Scheme_Object *prev_def;

    DEBUG(printf("deferring closure at position %d\n", position));
    if (DEBUG_LEVEL > 1) {
        printf(" outer: ");
        print_frame(outer);
        printf("\n inner: ");
        print_frame(inner);
        printf("\n");
    }

    if (position >= outer->count) {
        scheme_signal_error("update_frame: position exceeds binding count");
    }

    /* put the deferred expression in the right place */
    prev_def = (outer->def)[position];
    prev_def = scheme_make_pair((Scheme_Object *)clos, prev_def); 
    (outer->def)[position] = prev_def; 
     
    /* increment the waiting count between the current frame and the
       outer frame */
    for (; outer != inner; inner = inner->next) {
        (inner->waiting)++;
    }
    
    return;
}

/* records all the subexprs at the time of deferral */
Scheme_Object *frame_to_subexpr_ls(Letrec_Check_Frame *frame) {
    Scheme_Object *ls = scheme_null;

    for (; frame != NULL; frame = frame->next) {
        if (frame->subexpr < 0) {
            print_frame(frame);
            printf("\n");
            scheme_signal_error("frame_to_subexpr_ls: frame->subexpr is negative");
        }
        ls = scheme_make_pair(scheme_make_integer(frame->subexpr), ls);
    }
    
    return scheme_reverse(ls);
}

/* replaces all the subexprs to their state pre-deferral */
void subexpr_ls_to_frame(Scheme_Object *ls, Letrec_Check_Frame *frame) {
    for (; frame != NULL; frame = frame->next) {
        if (SCHEME_CAR(ls) < 0) {
            scheme_signal_error("subexpr_ls_to_frame: negative subexpr in list");
        }
        if (SCHEME_NULLP(ls)) {
            scheme_signal_error("subexpr_ls_to_frame: ls is null");
        }
        frame->subexpr = SCHEME_INT_VAL(SCHEME_CAR(ls));
        ls = SCHEME_CDR(ls);
    }
    
    return;
}

/* creates a deferred expression "closure" by closing over the frame,
   and uvars/pvars at the point of deferral */
Scheme_Deferred_Expr *make_deferred_expr_closure(Scheme_Closure_Data *expr, Letrec_Check_Frame *frame, 
                                                 int position, Scheme_Object *uvars, Scheme_Object *pvars)
{
    Scheme_Deferred_Expr *clos;

    Scheme_Object *subexpr_ls;
    subexpr_ls = frame_to_subexpr_ls(frame);

    clos = MALLOC_ONE_RT(Scheme_Deferred_Expr);
    clos->so.type = scheme_deferred_expr_type;
    clos->expr = expr;
    clos->frame = frame;
    clos->position = position;
    clos->uvars = uvars;
    clos->pvars = pvars;
    clos->subexpr_ls = subexpr_ls;

    return clos;
}

Scheme_Object *letrec_check_expr(Scheme_Object *, Letrec_Check_Frame *, 
                                 Scheme_Object *, Scheme_Object *, Scheme_Object *);
void process_deferred_bindings(Letrec_Check_Frame *);

void letrec_check_lets_resume(Letrec_Check_Frame *frame)
{
    Scheme_Compiled_Let_Value *clv;
    Scheme_Object *body;
    int i, j, k, *clv_flags;
    Scheme_Let_Header *head;
    int was_checked;

    DEBUG(printf("letrec_check_lets_resume\n "));

#ifdef MZTAG_REQUIRED
    if (frame->type != scheme_rt_letrec_check_frame) {
        scheme_signal_error("letrec_check_lets_resume: frame is not a frame");
    }
#endif

    head = frame->head;

    DEBUG(print_frame(frame));
    DEBUG(printf("\n attempting to resume, waiting is %d\n", frame->waiting));

    if (frame->waiting != 0) { 
        return; 
    }

    frame->subexpr = LET_BODY_EXPR;
    process_deferred_bindings(frame);
    frame->subexpr = -1;
    
    frame->subexpr = LET_NO_EXPR;
    process_deferred_bindings(frame);
    frame->subexpr = -1;

    body = head->body;
    if (frame->frame_type == FRAME_TYPE_LETREC) {
        DEBUG(printf("updating flags\n"));
        /* loops through every right hand side again to update the flags
           that we have invalidated; i.e., adding check-undefineds around
           references means there is one (more) instance where the LHS
           variable is not used in application position */
        k = head->count;
        for (i = head->num_clauses; i--;) {
            clv = (Scheme_Compiled_Let_Value *) body;
            clv_flags = clv->flags;
            k -= clv->count;
            for (j = 0; j < clv->count; j++) {
                was_checked = frame->checked[k + j];
                if (was_checked) {
                    DEBUG(printf("%d had check inserted\n", k));
                    clv_flags[j] -= (clv_flags[j] & SCHEME_WAS_ONLY_APPLIED);
                    clv_flags[j] -= (clv_flags[j] & SCHEME_WAS_APPLIED_EXCEPT_ONCE);
                } 
            }
            clv->flags = clv_flags;
            clv->names = NULL; /* not used in later passes */
            body = clv->body;
        }
    }

    return;
}

void print_vars(Scheme_Object *vars)
{
    fflush(stdout);
    scheme_display(vars, scheme_orig_stdout_port);
    scheme_flush_output(scheme_orig_stdout_port);

    return;
}


/* appends two nested lists of variables that are always the same length, e.x.
   merge_vars( ((1) () (0)) , (() (2) (1)) ) => ((1) (2) (0 1)) */
Scheme_Object *merge_vars(Scheme_Object *vars1, Scheme_Object *vars2) {
    Scheme_Object *merged, *car1, *car2, *appended_cars, *tmp;

    /* make sure they are the same length */
    if (scheme_proper_list_length(vars1) !=
        scheme_proper_list_length(vars2)) {
        scheme_signal_error("arguments to merge_vars are not the same length");
    }

    merged = scheme_null;
    while (!SCHEME_NULLP(vars1)) {
        if (SCHEME_NULLP(vars2)) {
            scheme_wrong_contract("merge_vars", "same-length?", -1, 0, &vars2);
        }

        car1 = SCHEME_CAR(vars1);
        car2 = SCHEME_CAR(vars2);

        appended_cars = scheme_append(car1, car2);
        merged = scheme_make_pair(appended_cars, merged);

        vars1 = SCHEME_CDR(vars1);
        vars2 = SCHEME_CDR(vars2);
    }

    tmp = scheme_reverse(merged);
    return tmp;
}

void check_inner_vars(Scheme_Object *ls) {
    while(!SCHEME_NULLP(ls)) {
        if (!SCHEME_PAIRP(ls)) {
            scheme_signal_error("check_inner_vars: vars is not a list");
        }
        ls = SCHEME_CDR(ls);
    }
    return;
}

void check_vars(Scheme_Object *vars) {
    DEBUG(printf("check_vars\n"));

    while(!SCHEME_NULLP(vars)) {
        if (!SCHEME_PAIRP(vars)) {
            scheme_signal_error("check_vars: vars is not a list");
        }
        check_inner_vars(SCHEME_CAR(vars));
        vars = SCHEME_CDR(vars);
    }

    return;
}

/* looks up an absolute position in a nested list of vars, where we
   only care about the outermost dimension; e.x.:

   lookup_var(1, ((1) ...)) = 1
   lookup_var(1, (() ...)) = 0
   lookup_var(2, ((1) ...)) = 0
*/
int lookup_var(int position, Scheme_Object *vars, Letrec_Check_Frame *frame) 
{
    Scheme_Object *vars_car, *caar;
    
    check_vars(vars);

    if (frame == NULL) {
        scheme_signal_error("lookup_var: frame == NULL");
        return 0;
    }

    if (DEBUG_LEVEL > 1) {
        printf("lookup_var: %d in ", position);
        print_vars(vars);
        printf(" and ");
        print_frame(frame);
        printf("\n");
    }

    if (SCHEME_NULLP(vars)) {
        return 0;
    }

    if (frame->subexpr < 0) {
        scheme_signal_error("lookup_var: subexpr is negative");
    }

    /* if we're in the RHS of a let, there are no bindings pushed yet
       so we don't have to do any offsetting */
    if ((frame->frame_type == FRAME_TYPE_LET) &&
        (frame->subexpr & LET_RHS_EXPR)) {
        return lookup_var(position, vars, frame->next);
    }
    if (position >= frame->count) {
        /* we're not in the right frame yet, so offset pos by the
           number of bindings in this frame */
        position -= frame->count;

        /* if is is not a letrec, or we are in the body of the letrec,
           there are no uvars/pvars for this frame, so don't cdr */
        if ((frame->frame_type != FRAME_TYPE_LETREC) ||
            (frame->subexpr & LET_BODY_EXPR)) {
            return lookup_var(position, vars, frame->next);
        } 
        else { 
            if (SCHEME_NULLP(vars)) {
                scheme_signal_error("lookup_var: vars is null");
            }
            return lookup_var(position, SCHEME_CDR(vars), frame->next); 
        }
    }

    if (frame->frame_type == FRAME_TYPE_LETREC) {
        if (frame->subexpr & LET_BODY_EXPR) {
            VERBOSE_DEBUG(printf(" didn't find %d in vars\n", position));
            return 0;
        }

        if (SCHEME_NULLP(vars)) {
            scheme_signal_error("lookup_var: vars is null");
        }

        /* we're in the right place, so we just have to check in the
           car of vars for the int we're looking for */
        vars_car = SCHEME_CAR(vars);
        while(!SCHEME_NULLP(vars_car)) {
            caar = SCHEME_CAR(vars_car);
            if (SCHEME_INT_VAL(caar) == position) {
                VERBOSE_DEBUG(printf(" found %d in vars\n", position));
                return 1;
            }
            vars_car = SCHEME_CDR(vars_car);
        }
    }

    VERBOSE_DEBUG(printf(" didn't find %d in vars\n", position));
    return 0;
}

/* records that we have seen a reference to loc */
void record_ref(Scheme_Local *loc, Letrec_Check_Frame *frame)
{
    Scheme_Object *deferred_with_rhs_ref, *deferred_with_body_ref;
    Letrec_Check_Frame *inner;
    int position = SCHEME_LOCAL_POS(loc);

    DEBUG(printf("recording reference for %d\n", position));

    inner = frame;
    frame = get_relative_frame(&position, frame);

    DEBUG(printf(" checking for LET_NO_EXPR\n"));
    for(; inner != frame; inner = inner->next) {
        if (inner->subexpr < 0) {
            scheme_signal_error("record_ref: subexpr is negative");
        }
        if (inner->subexpr & LET_NO_EXPR) {
            return;
        }
    }
    
    /* calculate the new flag to indicate we have seen loc in the
       subexpr of the letrec */

    frame->ref[position] |= frame->subexpr;

    deferred_with_rhs_ref = frame->deferred_with_rhs_ref;
    deferred_with_body_ref = frame->deferred_with_body_ref;
    if (!SCHEME_FALSEP(deferred_with_rhs_ref) || 
        !SCHEME_FALSEP(deferred_with_body_ref)) {
        Scheme_Object **def, *defls, *tmp;

        DEBUG(printf("record_ref: adding something to a frame deferred list\n"));

        def = frame->def;
        defls = def[position];
        
        if (!SCHEME_FALSEP(deferred_with_rhs_ref)) {
            tmp = scheme_append(defls, deferred_with_rhs_ref);
            frame->deferred_with_rhs_ref = tmp;
        }
        else {
            tmp = scheme_append(defls, deferred_with_body_ref);
            frame->deferred_with_body_ref = tmp;
        }

        DEBUG(printf("adding %d existing deferred expression(s) to deferred list\n",
                     scheme_list_length(def[position])));

        (frame->def)[position] = scheme_null;
    }

    return;
}

/* records that we have seen a reference to loc */
Scheme_Object *record_checked(Scheme_Local *loc, Letrec_Check_Frame *frame)
{
  int position = SCHEME_LOCAL_POS(loc), k;
    Scheme_Object *obj;

    frame = get_relative_frame(&position, frame);
    DEBUG(printf("recording check for (relative position) %d\n", position));
    DEBUG(print_frame(frame));
    DEBUG(printf("\n"));

    DEBUG(printf(" old value: %d\n", (frame->checked)[position]));

    (frame->checked)[position] = 1;

    DEBUG(printf(" new value: %d\n", (frame->checked)[position]));

    obj = frame->head->body;
    k = frame->head->count;

    while (1) {
      Scheme_Compiled_Let_Value *clv = (Scheme_Compiled_Let_Value *)obj;
      
      SCHEME_ASSERT(SAME_TYPE(SCHEME_TYPE(obj), scheme_compiled_let_value_type), "not a clv");
      SCHEME_ASSERT(position >= 0, "position went negative");
      
      k -= clv->count;

      if (position >= k)
        return clv->names[position - k];
      
      obj = clv->body;
    }

    ESCAPED_BEFORE_HERE;
}

/* returns another vars list that has the same length but has all
   empty lists
   
   rem_vars( ((1) (1 2) ) ) = (() ()) */
Scheme_Object *rem_vars(Scheme_Object *vars)
{
    Scheme_Object *tmp, *new;
    DEBUG(printf("rem_vars: removing vars from a list of vars\n"));

    new = scheme_null;
    tmp = vars;
    while(!SCHEME_NULLP(tmp)) {
        if (!SCHEME_PAIRP(tmp)) {
            scheme_wrong_contract("rem_vars", "list?", -1, 0, &tmp);
        }
        new = scheme_make_pair(scheme_null, new);
        tmp = SCHEME_CDR(tmp);
    }

    return new;
}

Scheme_Object *letrec_check_local(Scheme_Object *o, Letrec_Check_Frame *frame,
                                  Scheme_Object *uvars, Scheme_Object *pvars, 
                                  Scheme_Object *pos)
{
    Scheme_Local *loc = (Scheme_Local *)o;
    int position;

    DEBUG(printf("letrec_check_local\n"));

    position = SCHEME_LOCAL_POS(loc);

    if (DEBUG_LEVEL > 1) { 
        printf(" position: %d\n ", position);
        print_frame(frame);
        printf("\n uvars: ");
        print_vars(uvars);
        printf("\n pvars: ");
        print_vars(pvars);
        printf("\n");
    }

    /* record that we saw this local in the frame, so later we know to
       process its deferred bindings if there are any */
    record_ref(loc, frame);

    /* figure out if we need to add a check around this local
       reference; if it is neither protectable or unprotected, we do
       not have to add a check. */
    if (lookup_var(position, uvars, frame) ||
        lookup_var(position, pvars, frame)) {
        /* our reference is either unprotectable or protectable, so we
           need to insert an error check around it */
        Scheme_App3_Rec *app3;
        Scheme_Object *name;
        
        DEBUG(printf("adding a check around this reference because loc is %d\n", position));
        name = record_checked(loc, frame);
        
        app3 = MALLOC_ONE_TAGGED(Scheme_App3_Rec);
        app3->iso.so.type = scheme_application3_type;
        app3->rator = scheme_check_not_undefined_proc;
        app3->rand1 = o;
        app3->rand2 = name;
        
        return (Scheme_Object *) app3;
    }

    /* our reference is protected, so we're fine */
    DEBUG(printf("local was neither unprotected nor protectable\n"));
    return o;
}

Scheme_Object *letrec_check_application(Scheme_Object *o, Letrec_Check_Frame *frame, 
                                        Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    int i,n;
    Scheme_App_Rec *app;
    Scheme_Object *new_uvars, *new_pvars, *val;

    app = (Scheme_App_Rec *)o;

    /* we'll have to check the rator and all the arguments */
    n = 1 + app->num_args;

    /* by entering the sub-expressions of an application, all
       protectable variables are moved to the unprotected state. */
    new_uvars = merge_vars(uvars, pvars);
    new_pvars = rem_vars(pvars);
    pos = scheme_false;

    for (i = 0; i < n; i++) { 
        val = letrec_check_expr(app->args[i], frame, new_uvars, new_pvars, pos);
        app->args[i] = val;
    }

    return o;
}

Scheme_Object *letrec_check_application2(Scheme_Object *o, Letrec_Check_Frame *frame, 
                                         Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    Scheme_App2_Rec *app;
    Scheme_Object *new_uvars, *new_pvars, *val;

    app = (Scheme_App2_Rec *)o;
    
    /* by entering the sub-expressions of an application, all
       protectable variables are moved to the unprotected state. */
    new_uvars = merge_vars(uvars, pvars);
    new_pvars = rem_vars(pvars);
    pos = scheme_false;

    val = letrec_check_expr(app->rator, frame, new_uvars, new_pvars, pos);
    app->rator = val;
    val = letrec_check_expr(app->rand,  frame, new_uvars, new_pvars, pos);
    app->rand = val;

    return o;
}

Scheme_Object *letrec_check_application3(Scheme_Object *o, Letrec_Check_Frame *frame, 
                                         Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    Scheme_App3_Rec *app;
    Scheme_Object *new_uvars, *new_pvars, *val;

    app = (Scheme_App3_Rec *)o;

    /* by entering the sub-expressions of an application, all
       protectable variables are moved to the unprotected state. */
    new_uvars = merge_vars(uvars, pvars);
    new_pvars = rem_vars(pvars);
    pos = scheme_false;

    val = letrec_check_expr(app->rator, frame, new_uvars, new_pvars, pos);
    app->rator = val;
    val = letrec_check_expr(app->rand1, frame, new_uvars, new_pvars, pos);
    app->rand1 = val;
    val = letrec_check_expr(app->rand2, frame, new_uvars, new_pvars, pos);
    app->rand2 = val;

    return o;
}

Scheme_Object *letrec_check_sequence(Scheme_Object *o, Letrec_Check_Frame *frame, 
                                     Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    Scheme_Sequence *seq;
    Scheme_Object *val;
    int i,n;

    seq = (Scheme_Sequence *)o;

    n = seq->count;
    for (i = 0; i < n; i++) {
        val = letrec_check_expr(seq->array[i], frame, uvars, pvars, pos);
        seq->array[i] = val;
    }

    return o;
}

Scheme_Object *letrec_check_branch(Scheme_Object *o, Letrec_Check_Frame *frame, 
                                   Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    Scheme_Branch_Rec *br;
    Scheme_Object *val;

    br = (Scheme_Branch_Rec *)o;

    val = letrec_check_expr(br->test, frame, uvars, pvars, pos);
    br->test = val;
    val = letrec_check_expr(br->tbranch, frame, uvars, pvars, pos);
    br->tbranch = val;
    val = letrec_check_expr(br->fbranch, frame, uvars, pvars, pos);
    br->fbranch = val;
    
    return o;
}

Scheme_Object *letrec_check_wcm(Scheme_Object *o, Letrec_Check_Frame *frame, 
                                Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    Scheme_With_Continuation_Mark *wcm;
    Scheme_Object *val;

    wcm = (Scheme_With_Continuation_Mark *)o;
    
    val = letrec_check_expr(wcm->key, frame, uvars, pvars, pos);
    wcm->key = val;
    val = letrec_check_expr(wcm->val, frame, uvars, pvars, pos);
    wcm->val = val;
    val = letrec_check_expr(wcm->body, frame, uvars, pvars, pos);
    wcm->body = val;
    
    return o;
}

Scheme_Object *letrec_check_closure_compilation(Scheme_Object *o, Letrec_Check_Frame *frame, 
                                                Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    Scheme_Closure_Data *data;
    Letrec_Check_Frame *new_frame;
    Scheme_Object *new_pvars, *val;
    int num_params;

    data = (Scheme_Closure_Data *)o;

    /* if we have not entered a letrec, pos will be false */
    if (SCHEME_FALSEP(pos)) {
        /* by entering an lambda, we move all protectable variables to
           the protected state; i.e. we discard them since we do not
           track protected variables.  TODO: In reality, pvars is
           already null? */
        new_pvars = rem_vars(pvars);
        
        num_params = data->num_params;
        new_frame = init_letrec_check_frame(FRAME_TYPE_CLOSURE, num_params, frame, NULL);

#ifdef MZTAG_REQUIRED
        if ((frame != NULL) && (frame->type != scheme_rt_letrec_check_frame)) {
            scheme_signal_error("letrec_check_closure_compilation: frame is not a frame");
        }
        if (new_frame->type != scheme_rt_letrec_check_frame) {
            scheme_signal_error("letrec_check_closure_compilation: frame is not a frame");
        }
#endif

        if (num_params < 0) {
            scheme_signal_error("lambda has negative arguments what do");
        }
        
        pvars = new_pvars;
        val = letrec_check_expr(data->code, new_frame, uvars, pvars, pos);
        data->code = val;
        if (DEBUG_LEVEL > 1) {
            printf("letrec_check_closure_compilation: ");
            print_frame(new_frame);
            printf(" <- ");
            print_frame(frame);
            printf("\n");
        }

    }
    else {
        /* we can defer this lambda because it is not inside an
           application! hurray! */

        Scheme_Deferred_Expr *clos;
        Letrec_Check_Frame *outer_frame;
    
        /* create a deferred expression that closes over the frame it
           appeared in, the variable to which it is being deferred,
           and the current uvars and pvars */
        int position = SCHEME_INT_VAL(pos);
        clos = make_deferred_expr_closure(data, frame, position, uvars, pvars);

        /* get the correct frame and stick it up there */
        outer_frame = get_nearest_rhs(frame);
        update_frame(outer_frame, frame, position, clos);

    }

    return o;
}

void letrec_check_deferred_expr(Scheme_Object *o, Letrec_Check_Frame *outer, int type)
{
    Scheme_Deferred_Expr *clos;
    Scheme_Closure_Data *data;
    Letrec_Check_Frame *inner, *new_frame;
    Scheme_Object *tmp, *val, *uvars, *pvars, *tmp_uvars, *tmp_pvars, *deferred_uvars, *deferred_pvars, *after_i, *i_wrapped, *subexpr_ls;
    int i, old_subexpr, num_params, length_diff, position;

    /* gets the closed over data from clos, which will always be a
       deferred expression that contains a closure */
    clos = (Scheme_Deferred_Expr *)o;

    SCHEME_ASSERT(SAME_TYPE(SCHEME_TYPE(clos), scheme_deferred_expr_type),
                  "letrec_check_deferred_expr: clos is not a scheme_deferred_expr");

    data = (Scheme_Closure_Data *)clos->expr;
    position = clos->position;
    inner = clos->frame;
    uvars = clos->uvars;
    pvars = clos->pvars;
    subexpr_ls = clos->subexpr_ls;

    subexpr_ls_to_frame(subexpr_ls, inner);
    
#ifdef MZTAG_REQUIRED
        if (outer->type != scheme_rt_letrec_check_frame) {
            scheme_signal_error("letrec_check_deferred_expr: frame is not a frame");
        }
#endif

    if (scheme_proper_list_length(uvars) !=
        scheme_proper_list_length(pvars)) {
        scheme_signal_error("letrec_check_deferred_expr: vars different lengths");
    }

    DEBUG(printf(" preparing to process deferred expression for %d in subexpr %d\n", 
                 position, outer->subexpr));

    after_i = scheme_null;
    for (i = position - 1; i >= 0; i--) {
        i_wrapped = scheme_make_integer(i);
        tmp = scheme_make_pair(i_wrapped, after_i);
        after_i = tmp;
    }

    if (outer->frame_type == FRAME_TYPE_LETREC) {
        if (SCHEME_NULLP(uvars)) {
            scheme_signal_error("letrec_check_deferred_expr: uvars is null");
        }

        if (SCHEME_NULLP(pvars)) {
            scheme_signal_error("letrec_check_deferred_expr: pvars is null");
        }

        if (type & LET_NO_EXPR) {
            /* otherwise, it is not referenced anywhere in an unsafe context,
               so we're pretty much good.  still have to check it for letrecs
               in its sub-expressions */
            DEBUG(printf(" building NO environment\n"));

            deferred_uvars = scheme_make_pair(scheme_null, SCHEME_CDR(uvars));
            tmp = rem_vars(SCHEME_CDR(pvars));
            deferred_pvars = scheme_make_pair(scheme_null, tmp);
        }
        else if (type & LET_RHS_EXPR) { 
            /* the worst thing that can happen is that a LHS variable is
               referenced during the evaluation of a RHS binding (i.e. in
               an unsafe context in a RHS) so we check those first.
               
               we treat 1 thru i as protected, because this reference must
               occur in a binding after i (and therefore 1 thru i have
               values).  the rest of the LHS variables and those LHS
               variables from outer letrecs are considered unprotected.
            */
            DEBUG(printf(" building RHS environment\n"));

            deferred_uvars = merge_vars(uvars, pvars);
            tmp = scheme_make_pair(after_i, SCHEME_CDR(deferred_uvars));
            deferred_uvars = tmp;
            deferred_pvars = rem_vars(pvars);
        }
        
        else if (type & LET_BODY_EXPR) {
            /* the next worst thing that can happen is that a LHS variable
               is referenced during the body, where a variable from an
               outer letrec might appear.
               
               all LHS variables of the current letrec are protected, but
               the LHS variables from outer letrecs are unprotected.
            */
            DEBUG(printf(" building BODY environment\n"));

            tmp = scheme_make_pair(scheme_null, SCHEME_CDR(uvars));
            deferred_uvars = tmp;
            
            tmp = scheme_make_pair(scheme_null, SCHEME_CDR(pvars));
            deferred_pvars = tmp;
            
            tmp = merge_vars(deferred_uvars, deferred_pvars);
            deferred_uvars = tmp;
            deferred_pvars = rem_vars(deferred_pvars);
        }

        else {
            scheme_signal_error("letrec_check_deferred_expr: invalid type");
            return; // dead
        }
    }
    else {
        if (type & LET_NO_EXPR) {
            deferred_uvars = uvars;
            deferred_pvars = rem_vars(pvars);
        }
        else {
            deferred_uvars = uvars;
            deferred_pvars = pvars;
        }
    }
        
    /* we trust the enclosing let to know the statuses of variables up
       until (and including) it's own LHS variables, and then we
       switch over to the deferred expression's environment.  so, we
       compute the length difference of the two lists and chop off
       what we need to */
    length_diff = scheme_list_length(uvars) - scheme_list_length(deferred_uvars);
    
    tmp_uvars = scheme_null;
    tmp_pvars = scheme_null;
    while (length_diff > 0) {
        tmp = scheme_make_pair(SCHEME_CAR(uvars), tmp_uvars);
        tmp_uvars = tmp;
        tmp = scheme_make_pair(SCHEME_CAR(pvars), tmp_pvars);
        tmp_pvars = tmp;
        uvars = SCHEME_CDR(uvars);
        pvars = SCHEME_CDR(pvars);
        length_diff--;
    }
    tmp_uvars = scheme_reverse(tmp_uvars);
    tmp_pvars = scheme_reverse(tmp_pvars);

    uvars = scheme_append(tmp_uvars, deferred_uvars);
    pvars = scheme_append(tmp_pvars, deferred_pvars);

    DEBUG(printf(" calculated: uvars: "));
    DEBUG(print_vars(uvars);)
    DEBUG(printf(" pvars: "));
    DEBUG(print_vars(pvars);)
    DEBUG(printf("\n"));
    
    /* establish that we actually get a lambda back */
    if (SCHEME_TYPE(data) != scheme_compiled_unclosed_procedure_type) {
        printf("SCHEME_TYPE(data): %d\n", SCHEME_TYPE(data));
        scheme_signal_error("deferred expression does not contain a lambda");
    }

    /* hopefully we know how to deal with this lambda */
    num_params = data->num_params;
    if (num_params < 0) {
        scheme_signal_error("process_deferred_bindings: lambda has negative arguments");
    }

    if (outer->subexpr < 0) {
        scheme_signal_error("letrec_check_deferred_expr: subexpr is negative");
    }
    if (inner->subexpr < 0) {
        scheme_signal_error("letrec_check_deferred_expr: subexpr is negative");
    }

    old_subexpr = outer->subexpr;
    outer->subexpr = LET_RHS_EXPR;

    new_frame = init_letrec_check_frame(FRAME_TYPE_CLOSURE, num_params, inner, NULL);
    if (type & LET_NO_EXPR) {
        new_frame->subexpr |= LET_NO_EXPR;
    }

#ifdef MZTAG_REQUIRED
        if (new_frame->type != scheme_rt_letrec_check_frame) {
            scheme_signal_error("letrec_check_deferred_expr: frame is not a frame");
        }
        if (inner->type != scheme_rt_letrec_check_frame) {
            scheme_signal_error("letrec_check_deferred_expr: frame is not a frame");
        }
#endif

    val = letrec_check_expr(data->code, new_frame, uvars, pvars, scheme_false);
    data->code = val;

    outer->subexpr = old_subexpr;

#ifdef MZTAG_REQUIRED
    if (outer->type != scheme_rt_letrec_check_frame) {
        scheme_signal_error("letrec_check_deferred_expr: frame is not a frame");
    }
    if (new_frame->type != scheme_rt_letrec_check_frame) {
        scheme_signal_error("letrec_check_deferred_expr: frame is not a frame");
    }
#endif

    /* decrement the waiting count between the current frame and the
       outer frame */
    DEBUG(printf(" expression processed, checking between %p and %p\n", outer, inner));
    for (; outer != inner; inner = inner->next) {
        DEBUG(printf(" %p decrementing waiting and resuming letrecs\n", inner));
        if (inner == NULL) {
            scheme_signal_error("letrec_check_deferred_expr: inner is null");
        }
        if (outer == NULL) {
            scheme_signal_error("letrec_check_deferred_expr: outer is null");
        }
#ifdef MZTAG_REQUIRED
        if (inner->type != scheme_rt_letrec_check_frame) {
            scheme_signal_error("letrec_check_deferred_expr: frame is not a frame");
        }
        if (outer->type != scheme_rt_letrec_check_frame) {
            scheme_signal_error("letrec_check_deferred_expr: frame is not a frame");
        }
#endif
        (inner->waiting)--;
        letrec_check_lets_resume(inner);
    }

    DEBUG(printf("done waking things up\n"));
    return;
}

/* PLAN:
 * 
 * Indicate that we are traversing the RHSs of the let by changing the
 * frame->subexpr field, and then process every binding RHS.
 *
 * Populate frame->deferred_with_rhs_ref with the DEFERRED bindings of
 * every LHS variable mentioned in the RHSs according to frame->ref,
 * and process every expression in frame->deferred_with_rhs_ref.
 * 
 * If there is a reference to a LHS variable binding while
 * frame->deferred_with_rhs_ref is NOT FALSE, the deferred bindings
 * for that LHS variable are also placed in
 * frame->deferred_with_rhs_ref.  This step computes a closure.
 * 
 * Indicate that we are traversing the BODY of the let by changing the
 * frame->subexpr field, and then process the body.
 *
 * Populate frame->deferred_with_body_ref with the DEFERRED bindings
 * of every LHS variable mentioned in the body according to
 * frame->ref, and process every expression in
 * frame->deferred_with_body_ref.
 * 
 * If there is a reference to a LHS variable binding while
 * frame->deferred_with_body_ref is NOT FALSE, the deferred bindings
 * for that LHS variable are also placed in
 * frame->deferred_with_body_ref.  This step computes a closure.
 * 
 * Go back and remove the SCHEME_WAS_ONLY_APPLIED and
 * SCHEME_WAS_APPLIED_EXCEPT_ONCE flags from variables who had
 * undefined checks added around them according to frame->checked.
 */

/* populates frame->deferred_with_(rhs/body)_ref with the deferred
   binding of every LHS variable mentioned in the (RHSs/BODY)
   according to frame->ref, and processes every expression inside */
void process_deferred_bindings_rhs(Letrec_Check_Frame *frame) {
    // ls = &(frame->deferred_with_rhs_ref);

    // TODO: anything weird here?
    Scheme_Object **def, *tmp;
    int i, count, subexpr;

    subexpr = frame->subexpr;
    def = frame->def;
    count = frame->count;

    frame->deferred_with_rhs_ref = scheme_null;

    def = frame->def;
    for (i = 0; i < count; i++) {
        if (!SCHEME_NULLP(def[i]) && pos_has_ref(i, frame, subexpr)) {
            DEBUG(printf(" adding bindings for %d\n", i));
            tmp = scheme_append(def[i], frame->deferred_with_rhs_ref);
            frame->def[i] = scheme_null;
            frame->deferred_with_rhs_ref = tmp;
        }
        else if (SCHEME_NULLP(def[i])) {
            DEBUG(printf(" bindings for %d are null\n", i));
        }
        else if (!(pos_has_ref(i, frame, subexpr))) {
            DEBUG(printf(" no ref to %d\n", i));
        }
    }
    VERBOSE_DEBUG(printf(" %d expressions to be processed initially\n", 
                         scheme_list_length(frame->deferred_with_rhs_ref)));
    while (!SCHEME_NULLP(frame->deferred_with_rhs_ref)) {
        DEBUG(printf(" checking one expr\n"));
        if (!SCHEME_PAIRP(frame->deferred_with_rhs_ref)) {
            scheme_signal_error("process_deferred_bindings_rhs: ls is not a ls");
        }
        tmp = SCHEME_CAR(frame->deferred_with_rhs_ref);
        frame->deferred_with_rhs_ref = SCHEME_CDR(frame->deferred_with_rhs_ref);
        VERBOSE_DEBUG(printf(" popping one expression (%d left to be processed)\n",
                             scheme_list_length(frame->deferred_with_rhs_ref)));
        letrec_check_deferred_expr(tmp, frame, subexpr);
    }

    DEBUG(printf(" putting ls to false\n"));

#ifdef MZTAG_REQUIRED
    if (frame->type != scheme_rt_letrec_check_frame) {
        scheme_signal_error("process_deferred_bindings_rhs: frame is not a frame");
    }
#endif

    /* put the accumulator back to false before leaving */
    frame->deferred_with_rhs_ref = scheme_false;

    DEBUG(printf(" done processing deferred bindings\n"));
    
    return;
}

void process_deferred_bindings_body(Letrec_Check_Frame *frame) {
    // ls = &(frame->deferred_with_body_ref);

    Scheme_Object **def, *tmp;
    int i, count, subexpr;

    subexpr = frame->subexpr;
    def = frame->def;
    count = frame->count;

    frame->deferred_with_body_ref = scheme_null;

    def = frame->def;
    for (i = 0; i < count; i++) {
        if (!SCHEME_NULLP(def[i]) && pos_has_ref(i, frame, subexpr)) {
            DEBUG(printf(" adding bindings for %d\n", i));
            tmp = scheme_append(def[i], frame->deferred_with_body_ref);
            frame->def[i] = scheme_null;
            frame->deferred_with_body_ref = tmp;
        }
        else if (SCHEME_NULLP(def[i])) {
            DEBUG(printf(" bindings for %d are null\n", i));
        }
        else if (!(pos_has_ref(i, frame, subexpr))) {
            DEBUG(printf(" no ref to %d\n", i));
        }
    }
    VERBOSE_DEBUG(printf(" %d expressions to be processed initially\n", 
                         scheme_list_length(frame->deferred_with_body_ref)));
    while (!SCHEME_NULLP(frame->deferred_with_body_ref)) {
        DEBUG(printf(" checking one expr\n"));
        DEBUG(print_frame(frame));
        DEBUG(printf("\n"));
        if (!SCHEME_PAIRP(frame->deferred_with_body_ref)) {
            scheme_signal_error("process_deferred_bindings_body: ls is not a ls");
        }
        tmp = SCHEME_CAR(frame->deferred_with_body_ref);
        frame->deferred_with_body_ref = SCHEME_CDR(frame->deferred_with_body_ref);
        VERBOSE_DEBUG(printf(" popping one expression (%d left to be processed)\n",
                             scheme_list_length(frame->deferred_with_body_ref)));
        letrec_check_deferred_expr(tmp, frame, subexpr);
    }

    DEBUG(printf(" putting ls to false\n"));

#ifdef MZTAG_REQUIRED
    if (frame->type != scheme_rt_letrec_check_frame) {
        scheme_signal_error("process_deferred_bindings_body: frame is not a frame");
    }
#endif

    /* put the accumulator back to false before leaving */
    frame->deferred_with_body_ref = scheme_false;

    DEBUG(printf(" done processing deferred bindings\n"));
    
    return;
}

void process_deferred_bindings_no(Letrec_Check_Frame *frame) {
    // ls = &(frame->deferred_with_no_ref);

    Scheme_Object **def, *tmp;
    int i, count, subexpr;


    subexpr = frame->subexpr;
    def = frame->def;
    count = frame->count;

    frame->deferred_with_no_ref = scheme_null;
    def = frame->def;
    for (i = 0; i < count; i++) {
        if (!SCHEME_NULLP(def[i]) && pos_has_ref(i, frame, subexpr)) {
            DEBUG(printf(" adding bindings for %d\n", i));
            tmp = scheme_append(def[i], frame->deferred_with_no_ref);
            frame->def[i] = scheme_null;
            frame->deferred_with_no_ref = tmp;
        }
        else if (SCHEME_NULLP(def[i])) {
            DEBUG(printf(" bindings for %d are null\n", i));
        }
        else if (!(pos_has_ref(i, frame, subexpr))) {
            DEBUG(printf(" no ref to %d\n", i));
        }
    }
    VERBOSE_DEBUG(printf(" %d expressions to be processed initially\n", 
                         scheme_list_length(frame->deferred_with_no_ref)));
    while (!SCHEME_NULLP(frame->deferred_with_no_ref)) {
        DEBUG(printf(" checking one expr\n"));
        if (!SCHEME_PAIRP(frame->deferred_with_no_ref)) {
            scheme_signal_error("process_deferred_bindings_no: ls is not a ls");
        }
        tmp = SCHEME_CAR(frame->deferred_with_no_ref);
        frame->deferred_with_no_ref = SCHEME_CDR(frame->deferred_with_no_ref);
        VERBOSE_DEBUG(printf(" popping one expression (%d left to be processed)\n",
                             scheme_list_length(frame->deferred_with_no_ref)));
        letrec_check_deferred_expr(tmp, frame, subexpr);
    }

    DEBUG(printf(" putting ls to false\n"));

#ifdef MZTAG_REQUIRED
    if (frame->type != scheme_rt_letrec_check_frame) {
        scheme_signal_error("process_deferred_bindings_no: frame is not a frame");
    }
#endif

    /* put the accumulator back to false before leaving */
    frame->deferred_with_no_ref = scheme_false;

    DEBUG(printf(" done processing deferred bindings\n"));
    
    return;
}

void process_deferred_bindings(Letrec_Check_Frame *frame) {
    int subexpr;

    DEBUG(printf("processing deferred bindings\n"));
    if (DEBUG_LEVEL > 1) {
        printf(" ");
        print_frame(frame);
        printf("\n");
    }

    subexpr = frame->subexpr;

    VERBOSE_DEBUG(printf(" subexpr: %d\n", subexpr));
    
    if (subexpr & LET_NO_EXPR) {
        VERBOSE_DEBUG(printf(" checking deferred bindings with NO reference\n"));
        return process_deferred_bindings_no(frame);
    }
    else if (subexpr & LET_RHS_EXPR) {
        VERBOSE_DEBUG(printf(" checking deferred bindings with RHS reference\n"));
        return process_deferred_bindings_rhs(frame);
    } 
    else if (subexpr & LET_BODY_EXPR) {
        VERBOSE_DEBUG(printf(" checking deferred bindings with BODY reference\n"));
        return process_deferred_bindings_body(frame);
    }
    else {
        scheme_signal_error("process_deferred_bindings: unknown subexpr");
    }

    return; // dead
}

Scheme_Object *letrec_check_lets(Scheme_Object *o, Letrec_Check_Frame *old_frame, 
                                 Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    Letrec_Check_Frame *frame;
    Scheme_Compiled_Let_Value *clv;
    Scheme_Object *body, *new_uvars_level, *new_pvars_level, *i_wrapped, *tmp, *val;
    int i, j, k;

    /* gets the information out of our header about the number of
       total clauses, the number of total bindings, and whether or not
       this let is recursive */
    Scheme_Let_Header *head = (Scheme_Let_Header *)o;

    /* number of clauses in the let */
    int num_clauses = head->num_clauses;

    /* number of total bindings (not necessarily the same as the
       number of bindings thanks to let(rec)-values) */
    int count = head->count;

    /* information about this let */
    int header_flags = SCHEME_LET_FLAGS(head);

    /* computes what kind of let this is: letrec, let*, or let */
    int frame_type;

    Scheme_Object *new_uvars, *new_pvars;

    /* compute and record the type, either let let* or letrec */
    if (header_flags & SCHEME_LET_RECURSIVE) { 
        DEBUG(printf("LETREC_CHECK_LETREC\n"));
        frame_type = FRAME_TYPE_LETREC; 
    }
    else if (header_flags & SCHEME_LET_STAR) { 
        DEBUG(printf("LETREC_CHECK_LETSTAR\n"));
        frame_type = FRAME_TYPE_LETSTAR; 
    }
    else { 
        DEBUG(printf("LETREC_CHECK_LET\n"));
        frame_type = FRAME_TYPE_LET; 
    }

    /* push the new bindings on to the frame (even in the case of let
       this is ok because get_relative_frame knows how to look up
       variables properly given the subexpr */
    frame = init_letrec_check_frame(frame_type, count, old_frame, head);
#ifdef MZTAG_REQUIRED
    if (frame->type != scheme_rt_letrec_check_frame) {
        scheme_signal_error("letrec_check_lets: frame is not a frame");
    }
    if ((old_frame != NULL) && (old_frame->type != scheme_rt_letrec_check_frame)) {
        scheme_signal_error("letrec_check_lets: frame is not a frame");
    }
#endif

    /* add a new level to our uvars and pvars if this is a letrec */
    if (frame_type == FRAME_TYPE_LETREC) {
        new_pvars_level = scheme_null;
        new_uvars_level = scheme_null;
        
        for (i = 0; i < count; i++) {
            i_wrapped = scheme_make_integer(i);
            tmp = scheme_make_pair(i_wrapped, new_pvars_level);
            new_pvars_level = tmp;
        }

        // new_pvars_level = (i i-1 ... 1 0)
        // new_uvars_level = ()
    }

    body = head->body;

    frame->subexpr = LET_RHS_EXPR;

    /* loops through every right hand side */
    k = head->count;
    for (i = num_clauses; i--;) {
        clv = (Scheme_Compiled_Let_Value *)body;

        if (frame_type == FRAME_TYPE_LETREC) {
            new_uvars = scheme_make_pair(new_uvars_level, uvars);
            new_pvars = scheme_make_pair(new_pvars_level, pvars);
        }
        else {
            new_uvars = uvars;
            new_pvars = pvars;
        }

        k -= clv->count;

        /* TODO: is there something more sane for the treatment of
           bindings with no variables?  every lambda in the rhs will
           be treated as if there is an unsafe application where in
           fact it is just the opposite, no unsafe application can
           possibly happen */
        if (clv->count == 0) {
            val = letrec_check_expr(clv->value, frame, new_uvars, new_pvars,
                                    scheme_false);
        }
        else if (frame_type == FRAME_TYPE_LETREC) {
            val = letrec_check_expr(clv->value, frame, new_uvars, new_pvars,
                                    scheme_make_integer(k));
        }
        else {
            val = letrec_check_expr(clv->value, frame, new_uvars, new_pvars,
                                    pos);
        }
        
        if (frame_type == FRAME_TYPE_LETREC) {
            /* then remove the current LHS variable from the
               protectables variables as it is now protected */
            for (j = 0; j < clv->count; j++) {
                if (SCHEME_NULLP(new_pvars_level)) {
                    scheme_signal_error("letrec_check_lets: new_pvars_level is null");
                }
                new_pvars_level = SCHEME_CDR(new_pvars_level);
            }
        }
        
        if (frame_type == FRAME_TYPE_LETREC) {
            clv->value = val;
        }

        body = clv->body;
    }

    if (frame_type != FRAME_TYPE_LET) {
        process_deferred_bindings(frame);
    }

    /* body is already the right value thanks to the for */
    frame->subexpr = LET_BODY_EXPR;
    DEBUG(printf("processing let body\n"));
    val = letrec_check_expr(body, frame, uvars, pvars, pos);

    /* put the new body in the right place: after the last RHS if the
       let had bindings, otherwise, the let header should point to the
       new body */
    if (num_clauses > 0) clv->body = val;
    else head->body = val;

#ifdef MZTAG_REQUIRED
        if (frame->type != scheme_rt_letrec_check_frame) {
            scheme_signal_error("letrec_check_lets: frame is not a frame");
        }
#endif

    letrec_check_lets_resume(frame);

    DEBUG(printf("letrec_check_lets: "));
    DEBUG(print_frame(old_frame));
    DEBUG(printf(" <- "));
    DEBUG(print_frame(frame));
    DEBUG(printf("\n"));

    return o;
}

/* note to future self: the length of define_values is sometimes 1,
   and you definitely don't want to look inside if that's the case */
Scheme_Object *letrec_check_define_values(Scheme_Object *data, Letrec_Check_Frame *frame, 
                                          Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    if (SCHEME_VEC_SIZE(data) <= 1) {
        // TODO: right thing to do?
        return data;
    }
    else {
        Scheme_Object *vars = SCHEME_VEC_ELS(data)[0];
        Scheme_Object *val = SCHEME_VEC_ELS(data)[1];
        DEBUG(printf("letrec_check_define_values\n"));

        DEBUG(printf(" size: %d\n position 0: ", (int)SCHEME_VEC_SIZE(data)));
        DEBUG(fflush(stdout));
        DEBUG(scheme_display(vars, scheme_orig_stdout_port));
        DEBUG(scheme_flush_output(scheme_orig_stdout_port));
        DEBUG(printf("\n position 1: ")); 
        DEBUG(fflush(stdout));
        DEBUG(scheme_display(val, scheme_orig_stdout_port));
        DEBUG(scheme_flush_output(scheme_orig_stdout_port));
        DEBUG(printf("\n"));

        if(!SCHEME_PAIRP(vars) && !SCHEME_NULLP(vars)) {
            printf("vars: %d, val: %d\n", SCHEME_TYPE(vars), SCHEME_TYPE(val));
            scheme_signal_error("letrec_check_define_values: processing resolved code");
        }

        // TODO: don't think we need to process vars ever
        // vars = letrec_check_expr(vars, frame, uvars, pvars, pos);
        val = letrec_check_expr(val, frame, uvars, pvars, pos);

        SCHEME_VEC_ELS(data)[1] = val;
    }
    
    return data;
}

Scheme_Object *letrec_check_ref(Scheme_Object *data, Letrec_Check_Frame *frame, 
                                Scheme_Object *uvars, Scheme_Object *pvars, Wrapped_Lhs *lhs)
{
    return data;
}

Scheme_Object *letrec_check_set(Scheme_Object *o, Letrec_Check_Frame *frame, 
                                Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    Scheme_Set_Bang *sb;
    Scheme_Object *val;

    sb = (Scheme_Set_Bang *)o;
    val = sb->val;

    val = letrec_check_expr(val, frame, uvars, pvars, pos);
    sb->val = val;

    return o;
}

Scheme_Object *letrec_check_define_syntaxes(Scheme_Object *data, Letrec_Check_Frame *frame, 
                                            Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    Scheme_Object *val;
    val = SCHEME_VEC_ELS(data)[3];

    val = letrec_check_expr(val, frame, uvars, pvars, pos);
    SCHEME_VEC_ELS(data)[3] = val;

    return data;
}

Scheme_Object *letrec_check_begin_for_syntax(Scheme_Object *data, Letrec_Check_Frame *frame, 
                                             Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    Scheme_Object *l, *a, *val;
    
    l = SCHEME_VEC_ELS(data)[2];
    
    while (!SCHEME_NULLP(l)) {
        a = SCHEME_CAR(l);
        val = letrec_check_expr(a, frame, uvars, pvars, pos);
        SCHEME_CAR(l) = val;
        l = SCHEME_CDR(l);
    }
    
    return data;
}

Scheme_Object *letrec_check_case_lambda(Scheme_Object *o, Letrec_Check_Frame *frame, 
                                        Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    Scheme_Case_Lambda *cl;
    Scheme_Object *val;
    int i, n;

    cl = (Scheme_Case_Lambda *)o;

    n = cl->count;
    for (i = 0; i < n; i++) {
        val = letrec_check_expr(cl->array[i], frame, uvars, pvars, pos);
        cl->array[i] = val;
    }

    return o;
}

Scheme_Object *letrec_check_begin0(Scheme_Object *o, Letrec_Check_Frame *frame, 
                                   Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    int i, n;
    Scheme_Sequence *seq;
    Scheme_Object *val;
    
    seq = (Scheme_Sequence *)o;
    
    n = seq->count;
    for (i = 0; i < n; i++) {
        val = letrec_check_expr(seq->array[i], frame, uvars, pvars, pos);
        seq->array[i] = val;
    }

    return o;
}

Scheme_Object *letrec_check_apply_values(Scheme_Object *data, Letrec_Check_Frame *frame, 
                                         Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    Scheme_Object *f, *e;
    
    f = SCHEME_PTR1_VAL(data);
    e = SCHEME_PTR2_VAL(data);
    
    f = letrec_check_expr(f, frame, uvars, pvars, pos);
    e = letrec_check_expr(e, frame, uvars, pvars, pos);

    SCHEME_PTR1_VAL(data) = f;
    SCHEME_PTR2_VAL(data) = e;
    
    return data;
}

Scheme_Object *letrec_check_module(Scheme_Object *o, Letrec_Check_Frame *frame, 
                                   Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    int i, cnt;
    Scheme_Module *m;
    Scheme_Object *val;
    m = (Scheme_Module *)o;

    if (!m->comp_prefix) {
        /* already resolved */
        return (Scheme_Object *)m;
    }
    
    DEBUG(printf("letrec_check_module:\n "));
    MODNAME_DEBUG(fflush(stdout));
    MODNAME_DEBUG(scheme_display(m->modname, scheme_orig_stdout_port));
    MODNAME_DEBUG(scheme_flush_output(scheme_orig_stdout_port));
    MODNAME_DEBUG(printf("\n"));

    cnt = SCHEME_VEC_SIZE(m->bodies[0]);
    for(i = 0; i < cnt; i++) {
        val = SCHEME_VEC_ELS(m->bodies[0])[i];
        val = letrec_check_expr(val, frame, uvars, pvars, pos);
        SCHEME_VEC_ELS(m->bodies[0])[i] = val;
    }

    {
        /* check submodules */
        int k;
        Scheme_Object *p;
        for (k = 0; k < 2; k++) {
            p = (k ? m->post_submodules : m->pre_submodules);
            if (p) {
                while (!SCHEME_NULLP(p)) {
                    letrec_check_expr(SCHEME_CAR(p), frame, uvars, pvars, pos);
                    p = SCHEME_CDR(p);
                }
            }
        }
    }

    return o;
}

Scheme_Object *letrec_check_expr(Scheme_Object *expr, Letrec_Check_Frame *frame, 
                                 Scheme_Object *uvars, Scheme_Object *pvars, Scheme_Object *pos)
{
    int type;
    type = SCHEME_TYPE(expr);

    SCHEME_USE_FUEL(1);

    VERBOSE_DEBUG(printf ("letrec_check_expr: type %d\n", type));

    switch (type) {
    case scheme_local_type:
        return letrec_check_local(expr, frame, uvars, pvars, pos);
    case scheme_application_type:
        return letrec_check_application(expr, frame, uvars, pvars, pos);
    case scheme_application2_type:
        return letrec_check_application2(expr, frame, uvars, pvars, pos);
    case scheme_application3_type:
        return letrec_check_application3(expr, frame, uvars, pvars, pos);
    case scheme_sequence_type:
    case scheme_splice_sequence_type:
        return letrec_check_sequence(expr, frame, uvars, pvars, pos);
    case scheme_branch_type:
        return letrec_check_branch(expr, frame, uvars, pvars, pos);
    case scheme_with_cont_mark_type:
        return letrec_check_wcm(expr, frame, uvars, pvars, pos);
    case scheme_compiled_unclosed_procedure_type:
        return letrec_check_closure_compilation(expr, frame, uvars, pvars, pos);
    case scheme_compiled_let_void_type:
        return letrec_check_lets(expr, frame, uvars, pvars, pos);
    case scheme_compiled_toplevel_type: /* var ref to a top level */
        return expr;
    case scheme_compiled_quote_syntax_type:
        return expr;
    case scheme_variable_type:
    case scheme_module_variable_type:
        scheme_signal_error("got top-level in wrong place");
        return 0;
    case scheme_define_values_type:
        return letrec_check_define_values(expr, frame, uvars, pvars, pos);
    case scheme_varref_form_type:
        return letrec_check_ref(expr, frame, uvars, pvars, pos);
    case scheme_set_bang_type:
        return letrec_check_set(expr, frame, uvars, pvars, pos);
    case scheme_define_syntaxes_type:
        return letrec_check_define_syntaxes(expr, frame, uvars, pvars, pos);
    case scheme_begin_for_syntax_type:
        return letrec_check_begin_for_syntax(expr, frame, uvars, pvars, pos);
    case scheme_case_lambda_sequence_type:
        return letrec_check_case_lambda(expr, frame, uvars, pvars, pos);
    case scheme_begin0_sequence_type:
        return letrec_check_begin0(expr, frame, uvars, pvars, pos);
    case scheme_apply_values_type:
        return letrec_check_apply_values(expr, frame, uvars, pvars, pos);
    case scheme_require_form_type:
        return expr;
    case scheme_module_type:
        return letrec_check_module(expr, frame, uvars, pvars, pos);
    default:
        return expr;
    }
}

Scheme_Object *scheme_letrec_check_expr(Scheme_Object *expr)
{
    Scheme_Object *val;
    Scheme_Object *init_uvars = scheme_null;
    Scheme_Object *init_pvars = scheme_null;
    Scheme_Object *init_pos = scheme_false;

    DEBUG(printf("Entry\n"));
    val = letrec_check_expr(expr, NULL, init_uvars, init_pvars, init_pos);
    DEBUG(printf("Exit\n"));

    DEBUG(fflush(stdout));
    return val;
}

/*========================================================================*/
/*                         precise GC traversers                          */
/*========================================================================*/

#ifdef MZ_PRECISE_GC

START_XFORM_SKIP;

#include "mzmark_letrec_check.inc"

static void register_traversers(void)
{
  GC_REG_TRAV(scheme_rt_letrec_check_frame, mark_letrec_check_frame);
  GC_REG_TRAV(scheme_deferred_expr_type, mark_scheme_deferred_expr);
}

END_XFORM_SKIP;

#endif
