;; NOTE: Assertions have been generated by update_lit_checks.py and should not be edited.

;; Check that writing a struct field is not reordered with reading the same
;; struct field.

;; RUN: wasm-opt -all --simplify-locals %s -S -o - | filecheck %s
;; RUN: wasm-opt -all --simplify-locals %s --nominal -S -o - | filecheck %s --check-prefix=NOMNL

(module
  ;; CHECK:      (type $A (struct (field (mut i32))))
  ;; NOMNL:      (type $A (struct_subtype (field (mut i32)) data))
  (type $A (struct
    (field (mut i32))
  ))

  ;; Check that this:
  ;;
  ;;   y = a.0
  ;;   a.0 = 10
  ;;   return y
  ;;
  ;; Is not turned into this:
  ;;
  ;;   a.0 = 10
  ;;   return a.0
  ;;
  ;; CHECK:      (func $test (param $x (ref null $A)) (result i32)
  ;; CHECK-NEXT:  (local $y i32)
  ;; CHECK-NEXT:  (local.set $y
  ;; CHECK-NEXT:   (struct.get $A 0
  ;; CHECK-NEXT:    (local.get $x)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (struct.set $A 0
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:   (i32.const 10)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (local.get $y)
  ;; CHECK-NEXT: )
  ;; NOMNL:      (func $test (type $func.0) (param $x (ref null $A)) (result i32)
  ;; NOMNL-NEXT:  (local $y i32)
  ;; NOMNL-NEXT:  (local.set $y
  ;; NOMNL-NEXT:   (struct.get $A 0
  ;; NOMNL-NEXT:    (local.get $x)
  ;; NOMNL-NEXT:   )
  ;; NOMNL-NEXT:  )
  ;; NOMNL-NEXT:  (struct.set $A 0
  ;; NOMNL-NEXT:   (local.get $x)
  ;; NOMNL-NEXT:   (i32.const 10)
  ;; NOMNL-NEXT:  )
  ;; NOMNL-NEXT:  (local.get $y)
  ;; NOMNL-NEXT: )
  (func $test (export "test") (param $x (ref null $A)) (result i32)
    (local $y i32)
    (local.set $y
      (struct.get $A 0
        (local.get $x)
      )
    )
    (struct.set $A 0
      (local.get $x)
      (i32.const 10)
    )
    (local.get $y)
  )
)
