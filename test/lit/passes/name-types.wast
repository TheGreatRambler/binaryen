;; NOTE: Assertions have been generated by update_lit_checks.py --all-items and should not be edited.
;; RUN: wasm-opt %s -all --name-types -S -o - | filecheck %s
;; RUN: wasm-opt %s -all --name-types --nominal -S -o - | filecheck %s --check-prefix=NOMNL

(module
 ;; An obnoxious name that will be renamed.
 (type $obnoxious-super-long-type-name_____________________________1 (struct))

 ;; A reasonable name that will be kept.
 ;; CHECK:      (type $type$0 (func (param (ref $type$1) (ref $reasonable-name))))

 ;; CHECK:      (type $type$1 (struct))

 ;; CHECK:      (type $reasonable-name (struct (field i32)))
 ;; NOMNL:      (type $type$0 (func_subtype (param (ref $type$1) (ref $reasonable-name)) func))

 ;; NOMNL:      (type $type$1 (struct_subtype data))

 ;; NOMNL:      (type $reasonable-name (struct_subtype (field i32) data))
 (type $reasonable-name (struct (field i32)))

 ;; CHECK:      (func $foo (param $x (ref $type$1)) (param $y (ref $reasonable-name))
 ;; CHECK-NEXT:  (nop)
 ;; CHECK-NEXT: )
 ;; NOMNL:      (func $foo (type $type$0) (param $x (ref $type$1)) (param $y (ref $reasonable-name))
 ;; NOMNL-NEXT:  (nop)
 ;; NOMNL-NEXT: )
 (func $foo
  ;; Use the types to keep them alive.
  (param $x (ref $obnoxious-super-long-type-name_____________________________1))
  (param $y (ref $reasonable-name))
 )
)
