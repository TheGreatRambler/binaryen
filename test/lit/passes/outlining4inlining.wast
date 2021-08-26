;; NOTE: Assertions have been generated by update_lit_checks.py --all-items and should not be edited.

;; RUN: foreach %s %t wasm-opt --outlining-4-inlining --all-features -S -o - | filecheck %s

(module
  ;; CHECK:      (type $i32_=>_none (func (param i32)))

  ;; CHECK:      (type $none_=>_none (func))

  ;; CHECK:      (type $anyref_=>_none (func (param anyref)))

  ;; CHECK:      (import "out" "func" (func $import))
  (import "out" "func" (func $import))

  ;; CHECK:      (global $glob i32 (i32.const 1))
  (global $glob i32 (i32.const 1))

  ;; CHECK:      (func $maybe-work-hard (param $x i32)
  ;; CHECK-NEXT:  (if
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:   (call $maybe-work-hard$byn-outline
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $maybe-work-hard (param $x i32)
    ;; A function that does a quick check before any heavy work. We can outline
    ;; the heavy work, so that the condition can be inlined.
    (if
      (local.get $x)
      (return)
    )
    (loop $l
      (call $import)
      (br $l)
    )
  )

  ;; CHECK:      (func $condition-eqz (param $x i32)
  ;; CHECK-NEXT:  (if
  ;; CHECK-NEXT:   (i32.eqz
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (call $condition-eqz$byn-outline
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $condition-eqz (param $x i32)
    (if
      ;; More work in the condition, but work that we still consider worth
      ;; optimizing: a unary op.
      (i32.eqz
        (local.get $x)
      )
      (return)
    )
    (loop $l
      (call $import)
      (br $l)
    )
  )

  ;; CHECK:      (func $condition-global
  ;; CHECK-NEXT:  (if
  ;; CHECK-NEXT:   (global.get $glob)
  ;; CHECK-NEXT:   (call $condition-global$byn-outline)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $condition-global
    (if
      ;; A global read.
      (global.get $glob)
      (return)
    )
    (loop $l
      (call $import)
      (br $l)
    )
  )

  ;; CHECK:      (func $condition-ref.is (param $x anyref)
  ;; CHECK-NEXT:  (if
  ;; CHECK-NEXT:   (ref.is_null
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (call $condition-ref.is$byn-outline
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $condition-ref.is (param $x anyref)
    (if
      ;; A global read.
      (ref.is_null
        (local.get $x)
      )
      (return)
    )
    (loop $l
      (call $import)
      (br $l)
    )
  )

  ;; CHECK:      (func $condition-disallow-binary (param $x i32)
  ;; CHECK-NEXT:  (if
  ;; CHECK-NEXT:   (i32.add
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (return)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (loop $l
  ;; CHECK-NEXT:   (call $import)
  ;; CHECK-NEXT:   (br $l)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $condition-disallow-binary (param $x i32)
    (if
      ;; Work we do *not* allow (at least for now), a binary.
      (i32.add
        (local.get $x)
        (local.get $x)
      )
      (return)
    )
    (loop $l
      (call $import)
      (br $l)
    )
  )

  ;; CHECK:      (func $condition-disallow-unreachable (param $x i32)
  ;; CHECK-NEXT:  (if
  ;; CHECK-NEXT:   (i32.eqz
  ;; CHECK-NEXT:    (unreachable)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (return)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (loop $l
  ;; CHECK-NEXT:   (call $import)
  ;; CHECK-NEXT:   (br $l)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $condition-disallow-unreachable (param $x i32)
    (if
      ;; Work we do *not* allow (at least for now), an unreachable.
      (i32.eqz
        (unreachable)
      )
      (return)
    )
    (loop $l
      (call $import)
      (br $l)
    )
  )
)

;; CHECK:      (func $condition-ref.is$byn-outline (param $x anyref)
;; CHECK-NEXT:  (loop $l
;; CHECK-NEXT:   (call $import)
;; CHECK-NEXT:   (br $l)
;; CHECK-NEXT:  )
;; CHECK-NEXT: )

;; CHECK:      (func $condition-global$byn-outline
;; CHECK-NEXT:  (loop $l
;; CHECK-NEXT:   (call $import)
;; CHECK-NEXT:   (br $l)
;; CHECK-NEXT:  )
;; CHECK-NEXT: )

;; CHECK:      (func $condition-eqz$byn-outline (param $x i32)
;; CHECK-NEXT:  (loop $l
;; CHECK-NEXT:   (call $import)
;; CHECK-NEXT:   (br $l)
;; CHECK-NEXT:  )
;; CHECK-NEXT: )

;; CHECK:      (func $maybe-work-hard$byn-outline (param $x i32)
;; CHECK-NEXT:  (loop $l
;; CHECK-NEXT:   (call $import)
;; CHECK-NEXT:   (br $l)
;; CHECK-NEXT:  )
;; CHECK-NEXT: )
