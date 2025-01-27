;; NOTE: Assertions have been generated by update_lit_checks.py --all-items and should not be edited.
;; RUN: foreach %s %t wasm-opt -all --gufa            -S -o - | filecheck %s --check-prefix NO_OPT
;; RUN: foreach %s %t wasm-opt -all --gufa-optimizing -S -o - | filecheck %s --check-prefix DO_OPT

;; Compare the results of gufa and gufa-optimizing. The optimizing variant will
;; remove unneeded extra code that gufa might introduce, like dropped unneeded
;; things.

(module
  ;; NO_OPT:      (type $none_=>_i32 (func (result i32)))

  ;; NO_OPT:      (func $foo (result i32)
  ;; NO_OPT-NEXT:  (i32.const 1)
  ;; NO_OPT-NEXT: )
  ;; DO_OPT:      (type $none_=>_i32 (func (result i32)))

  ;; DO_OPT:      (func $foo (result i32)
  ;; DO_OPT-NEXT:  (i32.const 1)
  ;; DO_OPT-NEXT: )
  (func $foo (result i32)
    ;; Helper function.
    (i32.const 1)
  )

  ;; NO_OPT:      (func $bar (result i32)
  ;; NO_OPT-NEXT:  (drop
  ;; NO_OPT-NEXT:   (block $out (result i32)
  ;; NO_OPT-NEXT:    (block (result i32)
  ;; NO_OPT-NEXT:     (drop
  ;; NO_OPT-NEXT:      (block $in (result i32)
  ;; NO_OPT-NEXT:       (block (result i32)
  ;; NO_OPT-NEXT:        (drop
  ;; NO_OPT-NEXT:         (call $foo)
  ;; NO_OPT-NEXT:        )
  ;; NO_OPT-NEXT:        (i32.const 1)
  ;; NO_OPT-NEXT:       )
  ;; NO_OPT-NEXT:      )
  ;; NO_OPT-NEXT:     )
  ;; NO_OPT-NEXT:     (i32.const 1)
  ;; NO_OPT-NEXT:    )
  ;; NO_OPT-NEXT:   )
  ;; NO_OPT-NEXT:  )
  ;; NO_OPT-NEXT:  (i32.const 1)
  ;; NO_OPT-NEXT: )
  ;; DO_OPT:      (func $bar (result i32)
  ;; DO_OPT-NEXT:  (drop
  ;; DO_OPT-NEXT:   (call $foo)
  ;; DO_OPT-NEXT:  )
  ;; DO_OPT-NEXT:  (i32.const 1)
  ;; DO_OPT-NEXT: )
  (func $bar (result i32)
    ;; GUFA infers a constant value for each block here, adding multiple
    ;; constants of 1 and dropped earlier values. The optimizing variant of this
    ;; pass will avoid all that and just emit minimal code here (a drop of the
    ;; call followed by the value we inferred for it, 1).
    (block $out (result i32)
      (block $in (result i32)
        (call $foo)
      )
    )
  )
)
