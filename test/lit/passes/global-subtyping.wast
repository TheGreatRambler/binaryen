;; NOTE: Assertions have been generated by update_lit_checks.py --all-items and should not be edited.
;; RUN: foreach %s %t wasm-opt --nominal --global-subtyping -all -S -o - | filecheck %s
;; (remove-unused-names is added to test fallthrough values without a block
;; name getting in the way)

(module
  ;; This struct's field begins as a funcref, and can be specialized to a
  ;; particular typed function reference type because we only assign it a
  ;; ref.func of a particular function.
  ;; CHECK:      (type $ref|$struct|_=>_none (func (param (ref $struct))))

  ;; CHECK:      (type $struct (struct (field (mut (ref $ref|$struct|_=>_none)))))
  (type $struct (struct (field (mut funcref))))

  ;; CHECK:      (elem declare func $set)

  ;; CHECK:      (func $set (param $x (ref $struct))
  ;; CHECK-NEXT:  (struct.set $struct 0
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:   (ref.func $set)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $set (param $x (ref $struct))
    (struct.set $struct 0
      (local.get $x)
      (ref.func $set)
    )
  )
)

(module
  ;; As above, but we also assign a null funcref lower down, which prevents
  ;; specialization.
  ;; CHECK:      (type $struct (struct (field (mut funcref))))
  (type $struct (struct (field (mut funcref))))

  ;; CHECK:      (type $ref|$struct|_=>_none (func (param (ref $struct))))

  ;; CHECK:      (elem declare func $set)

  ;; CHECK:      (func $set (param $x (ref $struct))
  ;; CHECK-NEXT:  (struct.set $struct 0
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:   (ref.func $set)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (struct.set $struct 0
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:   (ref.null func)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $set (param $x (ref $struct))
    (struct.set $struct 0
      (local.get $x)
      (ref.func $set)
    )
    (struct.set $struct 0
      (local.get $x)
      (ref.null func)
    )
  )
)

(module
  ;; As above, but we also assign a different function reference. The type of
  ;; the field can be specialized to the LUB, which is a non-null ref to a func
  ;; (so we just specialized it to be non-null).
  ;; CHECK:      (type $struct (struct (field (mut (ref func)))))
  (type $struct (struct (field (mut funcref))))

  ;; CHECK:      (type $ref|$struct|_=>_none (func (param (ref $struct))))

  ;; CHECK:      (type $none_=>_none (func))

  ;; CHECK:      (elem declare func $other $set)

  ;; CHECK:      (func $set (param $x (ref $struct))
  ;; CHECK-NEXT:  (struct.set $struct 0
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:   (ref.func $set)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (struct.set $struct 0
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:   (ref.func $other)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $set (param $x (ref $struct))
    (struct.set $struct 0
      (local.get $x)
      (ref.func $set)
    )
    (struct.set $struct 0
      (local.get $x)
      (ref.func $other)
    )
  )

  ;; CHECK:      (func $other
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $other)
)

(module
  ;; CHECK:      (type $ref|$struct|_=>_none (func (param (ref $struct))))

  ;; CHECK:      (type $struct (struct (field (mut (ref $ref|$struct|_=>_none)))))
  (type $struct     (struct (field (mut funcref))))
  (type $sub-struct (struct (field (mut funcref))) (extends $struct))

  ;; CHECK:      (elem declare func $set)

  ;; CHECK:      (func $set (param $x (ref $struct))
  ;; CHECK-NEXT:  (struct.set $struct 0
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:   (ref.func $set)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $set (param $x (ref $struct))
    (struct.set $struct 0
      (local.get $x)
      (ref.func $set)
    )
  )
)

(module
  ;; We cannot specialize the type of a supertype's field without updating
  ;; that of the subtypes, as their fields must be subtypes. As we just write to
  ;; the supertype, here we will update them both.
  ;; CHECK:      (type $ref|$struct|_ref|$sub-struct|_=>_none (func (param (ref $struct) (ref $sub-struct))))

  ;; CHECK:      (type $struct (struct (field (mut (ref $ref|$struct|_ref|$sub-struct|_=>_none)))))
  (type $struct     (struct (field (mut funcref))))
  ;; CHECK:      (type $sub-struct (struct (field (mut (ref $ref|$struct|_ref|$sub-struct|_=>_none)))) (extends $struct))
  (type $sub-struct (struct (field (mut funcref))) (extends $struct))

  ;; CHECK:      (elem declare func $set)

  ;; CHECK:      (func $set (param $x (ref $struct)) (param $y (ref $sub-struct))
  ;; CHECK-NEXT:  (struct.set $struct 0
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:   (ref.func $set)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $set (param $x (ref $struct)) (param $y (ref $sub-struct))
    (struct.set $struct 0
      (local.get $x)
      (ref.func $set)
    )
  )
)

(module
  ;; As above, but we also write to the subtype. The write is compatible.

  ;; CHECK:      (type $ref|$struct|_ref|$sub-struct|_=>_none (func (param (ref $struct) (ref $sub-struct))))

  ;; CHECK:      (type $struct (struct (field (mut (ref $ref|$struct|_ref|$sub-struct|_=>_none)))))
  (type $struct     (struct (field (mut funcref))))
  ;; CHECK:      (type $sub-struct (struct (field (mut (ref $ref|$struct|_ref|$sub-struct|_=>_none)))) (extends $struct))
  (type $sub-struct (struct (field (mut funcref))) (extends $struct))

  ;; CHECK:      (elem declare func $set)

  ;; CHECK:      (func $set (param $x (ref $struct)) (param $y (ref $sub-struct))
  ;; CHECK-NEXT:  (struct.set $struct 0
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:   (ref.func $set)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (struct.set $sub-struct 0
  ;; CHECK-NEXT:   (local.get $y)
  ;; CHECK-NEXT:   (ref.func $set)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $set (param $x (ref $struct)) (param $y (ref $sub-struct))
    (struct.set $struct 0
      (local.get $x)
      (ref.func $set)
    )
    (struct.set $sub-struct 0
      (local.get $y)
      (ref.func $set)
    )
  )
)

(module
  ;; As above, but the write to the subtype is *not* compatible, which prevents
  ;; any optimization.

  ;; CHECK:      (type $struct (struct (field (mut funcref))))
  (type $struct     (struct (field (mut funcref))))
  ;; CHECK:      (type $sub-struct (struct (field (mut funcref))) (extends $struct))
  (type $sub-struct (struct (field (mut funcref))) (extends $struct))

  ;; CHECK:      (type $ref|$struct|_ref|$sub-struct|_=>_none (func (param (ref $struct) (ref $sub-struct))))

  ;; CHECK:      (elem declare func $set)

  ;; CHECK:      (func $set (param $x (ref $struct)) (param $y (ref $sub-struct))
  ;; CHECK-NEXT:  (struct.set $struct 0
  ;; CHECK-NEXT:   (local.get $x)
  ;; CHECK-NEXT:   (ref.func $set)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT:  (struct.set $sub-struct 0
  ;; CHECK-NEXT:   (local.get $y)
  ;; CHECK-NEXT:   (ref.null func)
  ;; CHECK-NEXT:  )
  ;; CHECK-NEXT: )
  (func $set (param $x (ref $struct)) (param $y (ref $sub-struct))
    (struct.set $struct 0
      (local.get $x)
      (ref.func $set)
    )
    (struct.set $sub-struct 0
      (local.get $y)
      (ref.null func)
    )
  )
)

(module
  ;; Only write to the subtype. As the fields are mutable, we cannot specialize
  ;; one without the other (if they are not 100% identical, the only way for
  ;; them to have a subtyping relationship is for them to both be immutable).
  ;; CHECK:      (type $ref|$struct|_ref|$sub-struct|_=>_none (func (param (ref $struct) (ref $sub-struct))))

  ;; CHECK:      (type $struct (struct (field (mut funcref))))
  (type $struct     (struct (field (mut funcref))))
  ;; CHECK:      (type $sub-struct (struct (field (mut funcref))) (extends $struct))
  (type $sub-struct (struct (field (mut funcref))) (extends $struct))

  ;; CHECK:      (func $set (param $x (ref $struct)) (param $y (ref $sub-struct))
  ;; CHECK-NEXT:  (nop)
  ;; CHECK-NEXT: )
  (func $set (param $x (ref $struct)) (param $y (ref $sub-struct))
    ;; FIXME
    ;; (struct.set $sub-struct 0
    ;;   (local.get $y)
    ;;   (ref.func $set)
    ;; )
  )
)

