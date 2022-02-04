;; NOTE: Assertions have been generated by update_lit_checks.py --all-items and should not be edited.
;; RUN: foreach %s %t wasm-opt --nominal --signature-refining -all -S -o - | filecheck %s

(module
  ;; $func is defined with an anyref parameter but always called with a $struct,
  ;; and we can specialize the heap type to that. That will both update the
  ;; heap type's definition as well as the types of the parameters as printed
  ;; on the function (which are derived from the heap type).

  ;; CHECK:      (type $struct (struct_subtype data))
  (type $struct (struct_subtype data))

  ;; CHECK:      (type $sig (func_subtype (param (ref $struct)) func))
  (type $sig (func_subtype (param anyref) func))

  ;; CHECK:      (type $func.0 (func_subtype func))

  ;; CHECK:      (func $func (type $sig) (param $x (ref $struct))
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $func (type $sig) (param $x anyref)
  )

  ;; CHECK:      (func $caller (type $func.0)
  ;; CHECK-NEXT:  (call $func
  ;; CHECK-NEXT:   (struct.new_default $struct)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $caller
    (call $func
      (struct.new $struct)
    )
  )
)

(module
  ;; As above, but the call is via call_ref.

  ;; CHECK:      (type $struct (struct_subtype data))
  (type $struct (struct_subtype data))

  ;; CHECK:      (type $sig (func_subtype (param (ref $struct)) func))
  (type $sig (func_subtype (param anyref) func))

  ;; CHECK:      (type $func.0 (func_subtype func))

  ;; CHECK:      (elem declare func $func)

  ;; CHECK:      (func $func (type $sig) (param $x (ref $struct))
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $func (type $sig) (param $x anyref)
  )

  ;; CHECK:      (func $caller (type $func.0)
  ;; CHECK-NEXT:  (call_ref
  ;; CHECK-NEXT:   (struct.new_default $struct)
  ;; CHECK-NEXT:   (ref.func $func)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $caller
    (call_ref
      (struct.new $struct)
      (ref.func $func)
    )
  )
)

(module
  ;; A combination of call types, and the LUB is affected by all of them: one
  ;; call uses a nullable $struct, the other a non-nullable dataref, so the LUB
  ;; is a nullable dataref.

  ;; CHECK:      (type $struct (struct_subtype data))
  (type $struct (struct_subtype data))

  ;; CHECK:      (type $sig (func_subtype (param (ref null data)) func))
  (type $sig (func_subtype (param anyref) func))

  ;; CHECK:      (type $func.0 (func_subtype func))

  ;; CHECK:      (elem declare func $func)

  ;; CHECK:      (func $func (type $sig) (param $x (ref null data))
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $func (type $sig) (param $x anyref)
  )

  ;; CHECK:      (func $caller (type $func.0)
  ;; CHECK-NEXT:  (local $struct (ref null $struct))
  ;; CHECK-NEXT:  (call $func
  ;; CHECK-NEXT:   (local.get $struct)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (call_ref
  ;; CHECK-NEXT:   (ref.as_data
  ;; CHECK-NEXT:    (struct.new_default $struct)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (ref.func $func)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $caller
    (local $struct (ref null $struct))
    (call $func
      ;; Use a local to avoid a ref.null being updated.
      (local.get $struct)
    )
    (call_ref
      (ref.as_data
        (struct.new $struct)
      )
      (ref.func $func)
    )
  )
)

(module
  ;; Multiple functions with the same heap type. Again, the LUB is in the
  ;; middle, this time the parent $struct and not a subtype.

  ;; CHECK:      (type $sig (func_subtype (param (ref $struct)) func))
  (type $sig (func_subtype (param anyref) func))

  ;; CHECK:      (type $func.0 (func_subtype func))

  ;; CHECK:      (type $struct-sub1 (struct_subtype $struct))
  (type $struct-sub1 (struct_subtype $struct))

  ;; CHECK:      (type $struct-sub2 (struct_subtype $struct))
  (type $struct-sub2 (struct_subtype $struct))

  ;; CHECK:      (type $struct (struct_subtype data))
  (type $struct (struct_subtype data))

  ;; CHECK:      (func $func-1 (type $sig) (param $x (ref $struct))
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $func-1 (type $sig) (param $x anyref)
  )

  ;; CHECK:      (func $func-2 (type $sig) (param $x (ref $struct))
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $func-2 (type $sig) (param $x anyref)
  )

  ;; CHECK:      (func $caller (type $func.0)
  ;; CHECK-NEXT:  (call $func-1
  ;; CHECK-NEXT:   (struct.new_default $struct-sub1)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (call $func-2
  ;; CHECK-NEXT:   (struct.new_default $struct-sub2)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $caller
    (call $func-1
      (struct.new $struct-sub1)
    )
    (call $func-2
      (struct.new $struct-sub2)
    )
  )
)

(module
  ;; As above, but now only one of the functions is called. The other is still
  ;; updated, though, as they share a heap type.

  ;; CHECK:      (type $sig (func_subtype (param (ref $struct)) func))
  (type $sig (func_subtype (param anyref) func))

  ;; CHECK:      (type $struct (struct_subtype data))
  (type $struct (struct_subtype data))

  ;; CHECK:      (type $func.0 (func_subtype func))

  ;; CHECK:      (func $func-1 (type $sig) (param $x (ref $struct))
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $func-1 (type $sig) (param $x anyref)
  )

  ;; CHECK:      (func $func-2 (type $sig) (param $x (ref $struct))
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $func-2 (type $sig) (param $x anyref)
  )

  ;; CHECK:      (func $caller (type $func.0)
  ;; CHECK-NEXT:  (call $func-1
  ;; CHECK-NEXT:   (struct.new_default $struct)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $caller
    (call $func-1
      (struct.new $struct)
    )
  )
)

(module
  ;; Define a field in the struct of the signature type that will be updated,
  ;; to check for proper validation after the update.

  ;; CHECK:      (type $sig (func_subtype (param (ref $struct)) func))
  (type $sig (func_subtype (param anyref) func))

  ;; CHECK:      (type $struct (struct_subtype (field (ref $sig)) data))
  (type $struct (struct_subtype (field (ref $sig)) data))

  ;; CHECK:      (type $func.0 (func_subtype func))

  ;; CHECK:      (elem declare func $func)

  ;; CHECK:      (func $func (type $sig) (param $x (ref $struct))
  ;; CHECK-NEXT:  (local $temp (ref null $sig))
  ;; CHECK-NEXT:  (local $2 anyref)
  ;; CHECK-NEXT:  (local.set $2
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (block
  ;; CHECK-NEXT:   (drop
  ;; CHECK-NEXT:    (local.get $2)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:   (local.set $2
  ;; CHECK-NEXT:    (local.get $temp)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $func (type $sig) (param $x anyref)
    ;; Define a local of the signature type that is updated.
    (local $temp (ref null $sig))
    ;; Do a local.get of the param, to verify its type is valid.
    (drop
      (local.get $x)
    )
    ;; Copy between the param and the local, to verify their types are still
    ;; compatible after the update. Note that we will need to add a fixup local
    ;; here, as $x's new type becomes too specific to be assigned the value
    ;; here.
    (local.set $x
      (local.get $temp)
    )
  )

  ;; CHECK:      (func $caller (type $func.0)
  ;; CHECK-NEXT:  (call $func
  ;; CHECK-NEXT:   (struct.new $struct
  ;; CHECK-NEXT:    (ref.func $func)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $caller
    (call $func
      (struct.new $struct
        (ref.func $func)
      )
    )
  )
)

(module
  ;; An unreachable value does not prevent optimization: we will update the
  ;; param to be $struct.

  ;; CHECK:      (type $struct (struct_subtype data))
  (type $struct (struct_subtype data))

  ;; CHECK:      (type $sig (func_subtype (param (ref $struct)) func))
  (type $sig (func_subtype (param anyref) func))

  ;; CHECK:      (type $func.0 (func_subtype func))

  ;; CHECK:      (elem declare func $func)

  ;; CHECK:      (func $func (type $sig) (param $x (ref $struct))
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $func (type $sig) (param $x anyref)
  )

  ;; CHECK:      (func $caller (type $func.0)
  ;; CHECK-NEXT:  (call $func
  ;; CHECK-NEXT:   (struct.new_default $struct)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (call_ref
  ;; CHECK-NEXT:   (unreachable)
  ;; CHECK-NEXT:   (ref.func $func)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $caller
    (call $func
      (struct.new $struct)
    )
    (call_ref
      (unreachable)
      (ref.func $func)
    )
  )
)

(module
  ;; When we have only unreachable values, there is nothing to optimize, and we
  ;; should not crash.

  (type $struct (struct_subtype data))

  ;; CHECK:      (type $sig (func_subtype (param anyref) func))
  (type $sig (func_subtype (param anyref) func))

  ;; CHECK:      (type $func.0 (func_subtype func))

  ;; CHECK:      (elem declare func $func)

  ;; CHECK:      (func $func (type $sig) (param $x anyref)
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $func (type $sig) (param $x anyref)
  )

  ;; CHECK:      (func $caller (type $func.0)
  ;; CHECK-NEXT:  (call_ref
  ;; CHECK-NEXT:   (unreachable)
  ;; CHECK-NEXT:   (ref.func $func)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $caller
    (call_ref
      (unreachable)
      (ref.func $func)
    )
  )
)

(module
  ;; When we have no calls, there is nothing to optimize, and we should not
  ;; crash.

  (type $struct (struct_subtype data))

  ;; CHECK:      (type $sig (func_subtype (param anyref) func))
  (type $sig (func_subtype (param anyref) func))

  ;; CHECK:      (func $func (type $sig) (param $x anyref)
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $func (type $sig) (param $x anyref)
  )
)

(module
  ;; Test multiple fields in multiple types.
  ;; CHECK:      (type $struct (struct_subtype data))
  (type $struct (struct_subtype data))

  ;; CHECK:      (type $sig-1 (func_subtype (param (ref null data) anyref) func))
  (type $sig-1 (func_subtype (param anyref) (param anyref) func))
  ;; CHECK:      (type $sig-2 (func_subtype (param anyref (ref $struct)) func))
  (type $sig-2 (func_subtype (param anyref) (param anyref) func))

  ;; CHECK:      (type $func.0 (func_subtype func))

  ;; CHECK:      (elem declare func $func-2)

  ;; CHECK:      (func $func-1 (type $sig-1) (param $x (ref null data)) (param $y anyref)
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $func-1 (type $sig-1) (param $x anyref) (param $y anyref)
  )

  ;; CHECK:      (func $func-2 (type $sig-2) (param $x anyref) (param $y (ref $struct))
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $func-2 (type $sig-2) (param $x anyref) (param $y anyref)
  )

  ;; CHECK:      (func $caller (type $func.0)
  ;; CHECK-NEXT:  (local $any anyref)
  ;; CHECK-NEXT:  (local $data (ref null data))
  ;; CHECK-NEXT:  (local $func funcref)
  ;; CHECK-NEXT:  (call $func-1
  ;; CHECK-NEXT:   (struct.new_default $struct)
  ;; CHECK-NEXT:   (local.get $data)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (call $func-1
  ;; CHECK-NEXT:   (local.get $data)
  ;; CHECK-NEXT:   (local.get $any)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (call $func-2
  ;; CHECK-NEXT:   (struct.new_default $struct)
  ;; CHECK-NEXT:   (struct.new_default $struct)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (call_ref
  ;; CHECK-NEXT:   (local.get $func)
  ;; CHECK-NEXT:   (struct.new_default $struct)
  ;; CHECK-NEXT:   (ref.func $func-2)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $caller
    (local $any (ref null any))
    (local $data (ref null data))
    (local $func (ref null func))

    (call $func-1
      (struct.new $struct)
      (local.get $data)
    )
    (call $func-1
      (local.get $data)
      (local.get $any)
    )
    (call $func-2
      (struct.new $struct)
      (struct.new $struct)
    )
    (call_ref
      (local.get $func)
      (struct.new $struct)
      (ref.func $func-2)
    )
  )
)

(module
  ;; The presence of a table prevents us from doing any optimizations.

  ;; CHECK:      (type $sig (func_subtype (param anyref) func))
  (type $sig (func_subtype (param anyref) func))

  ;; CHECK:      (type $func.0 (func_subtype func))

  ;; CHECK:      (type $struct (struct_subtype data))
  (type $struct (struct_subtype data))

  (table 1 1 anyref)

  ;; CHECK:      (table $0 1 1 anyref)

  ;; CHECK:      (func $func (type $sig) (param $x anyref)
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $func (type $sig) (param $x anyref)
  )

  ;; CHECK:      (func $caller (type $func.0)
  ;; CHECK-NEXT:  (call $func
  ;; CHECK-NEXT:   (struct.new_default $struct)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $caller
    (call $func
      (struct.new $struct)
    )
  )
)

(module
  ;; Pass a null in one call to the function. The null can be updated which
  ;; allows us to refine (but the new type must be nullable).

  ;; CHECK:      (type $struct (struct_subtype data))

  ;; CHECK:      (type $sig (func_subtype (param (ref null $struct)) func))
  (type $sig (func_subtype (param anyref) func))

  (type $struct (struct_subtype data))

  ;; CHECK:      (type $func.0 (func_subtype func))

  ;; CHECK:      (func $func (type $sig) (param $x (ref null $struct))
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $func (type $sig) (param $x anyref)
  )

  ;; CHECK:      (func $caller (type $func.0)
  ;; CHECK-NEXT:  (call $func
  ;; CHECK-NEXT:   (struct.new_default $struct)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (call $func
  ;; CHECK-NEXT:   (ref.null $struct)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $caller
    (call $func
      (struct.new $struct)
    )
    (call $func
      (ref.null data)
    )
  )
)

(module
  ;; CHECK:      (type $struct (struct_subtype data))
  (type $struct (struct_subtype data))

  ;; This signature has a single function using it, which returns a more
  ;; refined type, and we can refine to that.
  ;; CHECK:      (type $sig-can-refine (func_subtype (result (ref $struct)) func))
  (type $sig-can-refine (func_subtype (result anyref) func))

  ;; Also a single function, but no refinement is possible.
  ;; CHECK:      (type $sig-cannot-refine (func_subtype (result anyref) func))
  (type $sig-cannot-refine (func_subtype (result anyref) func))

  ;; The single function never returns, so no refinement is possible.
  ;; CHECK:      (type $sig-unreachable (func_subtype (result anyref) func))
  (type $sig-unreachable (func_subtype (result anyref) func))

  ;; CHECK:      (type $func.0 (func_subtype func))

  ;; CHECK:      (elem declare func $func-can-refine)

  ;; CHECK:      (func $func-can-refine (type $sig-can-refine) (result (ref $struct))
  ;; CHECK-NEXT:  (struct.new_default $struct)
  ;; CHECK-NEXT: )
  (func $func-can-refine (type $sig-can-refine) (result anyref)
    (struct.new $struct)
  )

  ;; CHECK:      (func $func-cannot-refine (type $sig-cannot-refine) (result anyref)
  ;; CHECK-NEXT:  (ref.null any)
  ;; CHECK-NEXT: )
  (func $func-cannot-refine (type $sig-cannot-refine) (result anyref)
    (ref.null any)
  )

  ;; CHECK:      (func $func-unreachable (type $sig-unreachable) (result anyref)
  ;; CHECK-NEXT:  (unreachable)
  ;; CHECK-NEXT: )
  (func $func-unreachable (type $sig-unreachable) (result anyref)
    (unreachable)
  )

  ;; CHECK:      (func $caller (type $func.0)
  ;; CHECK-NEXT:  (drop
  ;; CHECK-NEXT:   (if (result (ref $struct))
  ;; CHECK-NEXT:    (i32.const 1)
  ;; CHECK-NEXT:    (call $func-can-refine)
  ;; CHECK-NEXT:    (unreachable)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (drop
  ;; CHECK-NEXT:   (if (result (ref $struct))
  ;; CHECK-NEXT:    (i32.const 1)
  ;; CHECK-NEXT:    (call_ref
  ;; CHECK-NEXT:     (ref.func $func-can-refine)
  ;; CHECK-NEXT:    )
  ;; CHECK-NEXT:    (unreachable)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $caller
    ;; Add a call to see that we update call types properly.
    ;; Put the call in an if so the refinalize will update the if type and get
    ;; printed out conveniently.
    (drop
      (if (result anyref)
        (i32.const 1)
        (call $func-can-refine)
        (unreachable)
      )
    )
    ;; The same with a call_ref.
    (drop
      (if (result anyref)
        (i32.const 1)
        (call_ref
          (ref.func $func-can-refine)
        )
        (unreachable)
      )
    )
  )
)

(module
  ;; CHECK:      (type $struct (struct_subtype data))
  (type $struct (struct_subtype data))

  ;; This signature has multiple functions using it, and some of them have nulls
  ;; which should be updated when we refine.
  ;; CHECK:      (type $sig (func_subtype (result (ref null $struct)) func))
  (type $sig (func_subtype (result anyref) func))

  ;; CHECK:      (func $func-1 (type $sig) (result (ref null $struct))
  ;; CHECK-NEXT:  (struct.new_default $struct)
  ;; CHECK-NEXT: )
  (func $func-1 (type $sig) (result anyref)
    (struct.new $struct)
  )

  ;; CHECK:      (func $func-2 (type $sig) (result (ref null $struct))
  ;; CHECK-NEXT:  (ref.null $struct)
  ;; CHECK-NEXT: )
  (func $func-2 (type $sig) (result anyref)
    (ref.null any)
  )

  ;; CHECK:      (func $func-3 (type $sig) (result (ref null $struct))
  ;; CHECK-NEXT:  (ref.null $struct)
  ;; CHECK-NEXT: )
  (func $func-3 (type $sig) (result anyref)
    (ref.null eq)
  )

  ;; CHECK:      (func $func-4 (type $sig) (result (ref null $struct))
  ;; CHECK-NEXT:  (if
  ;; CHECK-NEXT:   (i32.const 1)
  ;; CHECK-NEXT:   (return
  ;; CHECK-NEXT:    (ref.null $struct)
  ;; CHECK-NEXT:   )
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (unreachable)
  ;; CHECK-NEXT: )
  (func $func-4 (type $sig) (result anyref)
    (if
      (i32.const 1)
      (return
        (ref.null any)
      )
    )
    (unreachable)
  )
)
