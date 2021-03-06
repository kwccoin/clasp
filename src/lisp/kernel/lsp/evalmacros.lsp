;;;;  -*- Mode: Lisp; Syntax: Common-Lisp; Package: SYSTEM -*-
;;;;
;;;;  Copyright (c) 1984, Taiichi Yuasa and Masami Hagiya.
;;;;  Copyright (c) 1990, Giuseppe Attardi.
;;;;  Copyright (c) 2001, Juan Jose Garcia Ripoll.
;;;;
;;;;    This program is free software; you can redistribute it and/or
;;;;    modify it under the terms of the GNU Library General Public
;;;;    License as published by the Free Software Foundation; either
;;;;    version 2 of the License, or (at your option) any later version.
;;;;
;;;;    See file '../Copyright' for full details.

(in-package :sys)

(eval-when (:compile-toplevel :load-toplevel :execute)
  (setq core::*debug-values* t))

(defmacro unless (pred &rest body)
  "Syntax: (unless test {form}*)
If TEST evaluates to NIL, then evaluates FORMs and returns all values of the
last FORM.  If not, simply returns NIL."
  `(IF (NOT ,pred) (PROGN ,@body)))

(defmacro defmacro (name lambda-list &body body &environment env)
  `(eval-when (:compile-toplevel :load-toplevel :execute)
     (let ((fn #',(ext:parse-macro name lambda-list body env)))
       (funcall #'(setf macro-function) fn ',name)
       (setf-lambda-list fn ',lambda-list)
       ,@(expand-set-documentation name 'function (find-documentation body))
       ',name)))

(defmacro destructuring-bind (vl list &body body)
  (multiple-value-bind (decls body)
      (find-declarations body)
    (multiple-value-bind (whole dl arg-check ignorables)
        (destructure vl nil)
      `(let* ((,whole ,list) ,@dl)
	 (declare (ignorable ,@ignorables))
         ,@decls
         ,@arg-check
         ,@body))))

(defmacro defvar (var &optional (form nil form-sp) doc-string)
  "Syntax: (defvar name [form [doc]])
Declares the variable named by NAME as a special variable.  If the variable
does not have a value, then evaluates FORM and assigns the value to the
variable.  FORM defaults to NIL.  The doc-string DOC, if supplied, is saved
as a VARIABLE doc and can be retrieved by (documentation 'NAME 'variable)."
  `(LOCALLY (DECLARE (SPECIAL ,var))
     (eval-when (:compile-toplevel :load-toplevel :execute)
       (SYS:*MAKE-SPECIAL ',var))
    ,@(when form-sp
	  `((UNLESS (BOUNDP ',var)
	      (SETQ ,var ,form))))
    ,@(si::expand-set-documentation var 'variable doc-string)
    ',var))

(defmacro defparameter (var form &optional doc-string)
  "Syntax: (defparameter name form [doc])
Declares the global variable named by NAME as a special variable and assigns
the value of FORM to the variable.  The doc-string DOC, if supplied, is saved
as a VARIABLE doc and can be retrieved by (documentation 'NAME 'variable)."
  `(LOCALLY (DECLARE (SPECIAL ,var))
     (eval-when (:compile-toplevel :load-toplevel :execute)
       (SYS:*MAKE-SPECIAL ',var))
     (SETQ ,var ,form)
     ,@(si::expand-set-documentation var 'variable doc-string)
     ',var))

;; export as extension?
(defmacro defconstant-eqx (var form test &optional doc-string)
  "Like DEFCONSTANT, but doesn't fire if the form is equal under TEST to an
existing value."
  (let ((value (gensym)))
    `(PROGN
       (eval-when (:compile-toplevel :load-toplevel :execute)
         (let ((,value ,form))
           (cond ((core:symbol-constantp ',var)
                  (unless (,test ,value (symbol-value ',var))
                    ;; This will just trigger the error in SET.
                    (set ',var ,value)))
                 ((core:specialp ',var)
                  (error "Cannot redefine special variable ~a as constant" ',var))
                 (t (set ',var ,value)
                    (funcall #'(setf core:symbol-constantp) t ',var)))))
       ,@(si::expand-set-documentation var 'variable doc-string)
       ',var)))

(defmacro defconstant (var form &optional doc-string)
  "Syntax: (defconstant symbol form [doc])

Declares that the global variable named by SYMBOL is a constant with the value
of FORM as its constant value.  The doc-string DOC, if supplied, is saved as a
VARIABLE doc and can be retrieved by (DOCUMENTATION 'SYMBOL 'VARIABLE)."
  `(defconstant-eqx ,var ,form eql ,doc-string))

(defmacro defconstant-equal (var form &optional doc-string)
  `(defconstant-eqx ,var ,form equal ,doc-string))

(export '(defconstant-equal))

(defmacro defun (&whole whole name vl &body body &environment env)
  ;; Documentation in help.lsp
  (multiple-value-bind (decls body doc-string) 
      (process-declarations body t)
    (let* ((loc (ext:current-source-location))
           (filepos (if loc (source-file-pos-filepos loc) 0))
           (lineno (if loc (source-file-pos-lineno loc) 0))
           (column (if loc (source-file-pos-column loc) 0))
           (fn (gensym))
           (doclist (when doc-string (list doc-string)))
           (global-function
             `#'(lambda ,vl 
                  (declare (core:lambda-name ,name core:current-source-file ,filepos ,lineno ,column) ,@decls) 
                  ,@doclist
                  (block ,(si::function-block-name name) ,@body))))
      ;;(bformat t "macro expansion of defun current-source-location -> %s\n" current-source-location)
      ;;(bformat t "DEFUN global-function --> %s\n" global-function )
      `(progn
         (eval-when (:compile-toplevel)
           ;; this function won't be ready for a while, but it's okay as there's no
           ;; compiler to run :compile-toplevel forms anyway.
           (cmp::register-global-function-def 'defun ',name))
         (let ((,fn ,global-function))
           (funcall #'(setf fdefinition) ,fn ',name)
           (setf-lambda-list ,fn ',vl)
           (core:set-source-info ,fn ',(list 'core:current-source-file filepos lineno column))
           ,@(si::expand-set-documentation name 'function doc-string)
           ;; This can't be at toplevel.
           ,@(and *defun-inline-hook*
                  (list (funcall *defun-inline-hook* name global-function env)))
           (setq cmp::*current-form-lineno* ,lineno)
           ',name)))))

;;;
;;; This is a no-op unless the compiler is installed
;;;
(defmacro bclasp-define-compiler-macro (&whole whole name vl &rest body &environment env)
  ;; CLHS doesn't actually say d-c-m has compile time effects, but it's nice to match defmacro
  `(eval-when (:compile-toplevel :load-toplevel :execute)
     (core:setf-bclasp-compiler-macro-function
      ',name
      (function ,(ext:parse-compiler-macro name vl body env)))
     ',name))

(defun bclasp-compiler-macro-function (name &optional env)
  ;;  (declare (ignorable env))
  (core:get-bclasp-compiler-macro-function name env))
;;  (values (get-sysprop name 'sys::compiler-macro)))

(export '(bclasp-define-compiler-macro bclasp-compiler-macro-function))

(defun bclasp-compiler-macroexpand-1 (form &optional env)
  (if (atom form)
      form
      (or
       (and (eq (car form) 'cl:funcall)
            (listp (cadr form))
            (eq (car (cadr form)) 'cl:function)
            (let ((expander (core:bclasp-compiler-macro-function (cadr (cadr form)) env)))
              (if expander
                  (funcall *macroexpand-hook* expander (cons (cadr (cadr form)) (cddr form)) env)
                  form)))
       (let ((expander (core:bclasp-compiler-macro-function (car form) env)))
         (if expander
             (funcall *macroexpand-hook* expander form env)
             form)))))

(defun bclasp-compiler-macroexpand (form &optional env)
  (let ((expansion (bclasp-compiler-macroexpand-1 form env)))
    (if (eq expansion form)
        (return-from bclasp-compiler-macroexpand form)
        (bclasp-compiler-macroexpand expansion env))))

(export '(bclasp-compiler-macroexpand-1 bclasp-compiler-macroexpand))

(defmacro define-compiler-macro (&whole whole name vl &rest body &environment env)
  ;; CLHS doesn't actually say d-c-m has compile time effects, but it's nice to match defmacro
  `(eval-when (:compile-toplevel :load-toplevel :execute)
     (funcall #'(setf compiler-macro-function)
              (function ,(ext:parse-compiler-macro name vl body env))
              ',name)
     ',name))

(defun compiler-macroexpand-1 (form &optional env)
  (if (atom form)
      form
      (or
       (and (eq (car form) 'cl:funcall)
            (listp (cadr form))
            (eq (car (cadr form)) 'cl:function)
            (let ((expander (compiler-macro-function (cadr (cadr form)) env)))
              (if expander
                  (funcall *macroexpand-hook* expander (cons (cadr (cadr form)) (cddr form)) env)
                  form)))
       (let ((expander (compiler-macro-function (car form) env)))
         (if expander
             (funcall *macroexpand-hook* expander form env)
             form)))))

(defun compiler-macroexpand (form &optional env)
  (let ((expansion (compiler-macroexpand-1 form env)))
    (if (eq expansion form)
        (return-from compiler-macroexpand form)
        (compiler-macroexpand expansion env))))

(export '(compiler-macroexpand-1 compiler-macroexpand))

;;; Each of the following macros is also defined as a special form,
;;; as required by CLtL. Some of them are used by the compiler (e.g.
;;; dolist), some not at all (e.g. defun).
;;; Thus their names need not be exported.

(let ()
  ;; We enclose the macro in a LET form so that it is no longer
  ;; a toplevel form. This solves the problem of this simple LOOP
  ;; replacing the more complex form in loop2.lsp when evalmacros.lsp
  ;; gets compiled.
(defmacro loop (&rest body &aux (tag (gensym)))
  "Syntax: (loop {form}*)
Establishes a NIL block and executes FORMs repeatedly.  The loop is normally
terminated by a non-local exit."
  `(BLOCK NIL (TAGBODY ,tag (PROGN ,@body) (GO ,tag)))))

(defmacro lambda (&rest body) `(function (lambda ,@body)))

; assignment

#-clasp-min
(defmacro psetq (&rest args)
  "Syntax: (psetq {var form}*)
Similar to SETQ, but evaluates all FORMs first, and then assigns each value to
the corresponding VAR.  Returns NIL."
  (BLOCK NIL
    (LET ((L ARGS) (FORMS NIL) (BINDINGS NIL))
      (TAGBODY
	 (GO bot)
       top
	 (TAGBODY
	    (LET ((SYM (GENSYM)))
	      (PUSH (LIST SYM (CADR L)) BINDINGS)
	      (PUSH (LIST 'SETQ (CAR L) SYM) FORMS)))
	 (SETQ L (CDDR L))
       bot
	 (UNLESS (ENDP L) (GO top))
	 (RETURN-FROM NIL
	   (PROGN
	     (LIST* 'LET* (NREVERSE BINDINGS) (NREVERSE (CONS NIL FORMS)))))))))


; conditionals

(defmacro cond (&rest clauses &aux (form nil))
  "Syntax: (cond {(test {form}*)}*)
Evaluates TESTs in order until one evaluates to non-NIL.  Then evaluates FORMs
in order that follow the TEST and returns all values of the last FORM.  If no
forms follow the TEST, then returns the value of the TEST.  Returns NIL, if no
TESTs evaluates to non-NIL."
  (dolist (l (reverse clauses) form)	; don't use nreverse here
    (if (endp (cdr l))
	(if (eq (car l) 't)
	    (setq form 't)
	    (let ((sym (gensym)))
	      (setq form `(LET ((,sym ,(car l)))
			   (IF ,sym ,sym ,form)))))
	(if (eq (car l) 't)
	    (setq form (if (endp (cddr l))
			   (cadr l)
			   `(PROGN ,@(cdr l))))
	    (setq form (if (endp (cddr l))
			   `(IF ,(car l) ,(cadr l) ,form)
			   `(IF ,(car l) (PROGN ,@(cdr l)) ,form)))))))

; program feature

(defmacro prog (vl &rest body &aux (decl nil))
  "Syntax: (prog ({var | (var [init])}*) {decl}* {tag | statement}*)
Establishes a NIL block, binds each VAR to the value of INIT (which defaults
to NIL) in parallel, and executes STATEMENTs.  Returns NIL."
  (multiple-value-setq (decl body)
    (find-declarations body))
  `(BLOCK NIL (LET ,vl ,@decl (TAGBODY ,@body))))

(defmacro prog* (vl &rest body &aux (decl nil))
  "Syntax: (prog* ({var | (var [init])}*) {decl}* {tag | statement}*)
Establishes a NIL block, binds each VAR to the value of INIT (which defaults
to NIL) sequentially, and executes STATEMENTs.  Returns NIL."
  (multiple-value-setq (decl body)
    (find-declarations body))
  `(BLOCK NIL (LET* ,vl ,@decl (TAGBODY ,@body))))

; sequencing

(defmacro prog1 (first &rest body &aux (sym (gensym)))
  "Syntax: (prog1 first-form {form}*)
Evaluates FIRST-FORM and FORMs in order.  Returns the value of FIRST-FORM."
  (if (null body)
      first
      `(LET ((,sym ,first))
         ,@body ,sym)))



(defmacro prog2 (first second &rest body &aux (sym (gensym)))
  "Syntax: (prog2 first-form second-form {forms}*)
Evaluates FIRST-FORM, SECOND-FORM, and FORMs in order.  Returns the value of
SECOND-FORM."
  `(PROGN ,first
          (LET ((,sym ,second))
            ,@body ,sym)))

; multiple values

(defmacro multiple-value-list (form)
  "Evaluates FORM and returns a list of all values FORM returns."
  `(MULTIPLE-VALUE-CALL #'LIST ,form))

(defmacro multiple-value-setq (vars form)
  "Syntax: (multiple-value-setq {var}* form)

Evaluates FORM and binds the N-th VAR to the N-th value of FORM or, if FORM
returns less than N values, to NIL.  Returns the first value of FORM or, if
FORM returns no value, NIL."
  (do ((vl vars (cdr vl))
       (sym (gensym))
       (forms nil)
       (n 0 (the fixnum (1+ n))))
      ((endp vl) `(LET ((,sym (MULTIPLE-VALUE-LIST ,form))) ,@forms))
    (declare (fixnum n))
    (push `(SETQ ,(car vl) (NTH ,n ,sym)) forms)))

(defmacro multiple-value-bind (vars form &rest body)
  "Syntax: (multiple-value-bind ({var}*) init {decl}* {form}*)

Evaluates INIT and binds the N-th VAR to the N-th value of INIT or, if INIT
returns less than N values, to NIL.  Then evaluates FORMs, and returns all
values of the last FORM.  If no FORM is given, returns NIL."
  (declare (notinline mapcar))
  (if (= (length vars) 1)
      ;; at the moment we don't handle multiple-value-call well, so this is probably
      ;; faster. Might be so in the future too.
      ;; Who would write m-v-b with one variable, you ask? Computers! (Mostly SETF.)
      `(let ((,(first vars) ,form)) ,@body)
      `(multiple-value-call #'(lambda (&optional ,@(mapcar #'list vars) &rest ,(gensym)) ,@body) ,form)))

(defun while-until (test body jmp-op)
  (let ((label (gensym))
	(exit (gensym)))
    `(TAGBODY
        (GO ,exit)
      ,label
        ,@body
      ,exit
	(,jmp-op ,test (GO ,label)))))

(defmacro sys::while (test &body body)
  (while-until test body 'when))

(defmacro sys::until (test &body body)
  (while-until test body 'unless))

(defmacro case (keyform &rest clauses)
  (let* ((last t)
	 (form nil)
	 (key (gensym)))
    (dolist (clause (reverse clauses)
	     `(LET ((,key ,keyform))
		,form))
      (let ((selector (car clause)))
	(cond ((or (eq selector T) (eq selector 'OTHERWISE))
	       (unless last
		 (si::signal-simple-error
		  'simple-program-error nil
		  "CASE: The selector ~A can only appear at the last position."
		  (list selector)))
	       (setq form `(PROGN ,@(cdr clause))))
	      ((consp selector)
	       (setq form `(IF (MEMBER ,key ',selector)
			       (PROGN ,@(cdr clause))
			       ,form)))
	      (selector
	       (setq form `(IF (EQL ,key ',selector)
			       (PROGN ,@(cdr clause))
			       ,form))))
	(setq last nil)))))

(defmacro return (&optional (val nil)) `(RETURN-FROM NIL ,val))

;; Declarations
(defmacro declaim (&rest decl-specs)
  `(eval-when (:compile-toplevel :load-toplevel :execute)
     ,@(mapcar #'(lambda (decl-spec)
                   `(proclaim ',decl-spec))
               decl-specs)))

(defmacro in-package (name)
  `(eval-when (:compile-toplevel :load-toplevel :execute)
     (si::select-package ,(string name))
     *package*))

(defun (setf symbol-macro) (expansion name)
  (put-sysprop name 'core:symbol-macro
               (lambda (form env)
                 (declare (ignore form env))
                 expansion)))

(defmacro define-symbol-macro (&whole whole symbol expansion)
  (cond ((not (symbolp symbol))
	 (error "DEFINE-SYMBOL-MACRO: ~A is not a symbol"
		symbol))
	((specialp symbol)
	 (error "DEFINE-SYMBOL-MACRO: cannot redefine a special variable, ~A"
		symbol))
	(t
	 `(eval-when (:compile-toplevel :load-toplevel :execute)
            (funcall #'(setf symbol-macro) ',expansion ',symbol)
	   ',symbol))))

(defmacro nth-value (n expr)
  `(nth ,n (multiple-value-list ,expr)))

(defun maybe-unquote (form)
  (if (and (consp form) (eq (car form) 'quote))
      (second form)
      form))
