# Nullable Types (`?T`) Implementation Plan

## Current State

| Component 10 of the plan mentions "05-language-surface.md" and "07-logwatcher-proof.md" which might think "wait, there's no types.ml yet" - that's Task 6, which is where nullable types will be properly handled.

### Existing Files
- **Lexer** (`lexer.ml`): ✅ Has `Question` token (`?`) - line 266
- **Token** (`token.ml`): Has `Question` kind - line 60
- **AST** (`ast.ml`): `field_ty` has `Scalar`, `Ref`, `Multi`, `Map` - **missing `Nullable`**
- **Parser** (`parser.ml`): `parse_field_ty` handles `ref`, `multi`, `map`, scalar - **doesn't handle `?` prefix**
- **Dump** (`dump.ml`): Handles existing field types - needs update
- **Typechecker** (`types.ml`): Not yet created (Task 6)

---

## Required Changes

### 1. AST (`ast.ml`) - Add Nullable Variant

**Minimal Change (Option A):**
```ocaml
type field_ty =
  | Scalar of string
  | Ref of string
  | Multi of string
  | Map of string * string
  | Nullable of field_ty   (* NEW: ?T wrapper *)
```

**Full Refactor (Option B - Recommended):**
```ocaml
type field_ty =
  | Scalar of string
  | Ref of string
  | Multi of string
  | Map of string * string
  | Nullable of field_ty   (* NEW: ?T wrapper *)

(* Update these to use field_ty for consistency *)
type param = {
  id : int;
  pos : pos;
  name : string;
  conv : param_conv;
  ty : field_ty;  (* was: string *)
}

type method_sig = {
  id : int;
  pos : pos;
  name : string;
  params : param list;
  ret : field_ty option;  (* was: string option *)
}

type method_decl = {
  ...
  ret : field_ty option;  (* was: string option *)
}
```

**Decision**: **Option B** - Full refactor for consistency. The typechecker (Task 6) needs resolved types anyway.

---

#### 3. Parser (`parser.ml`) - Parse `?` Prefix

**Changes needed in `parse_field_ty` (line 312):**
```ocaml
let parse_field_ty (st : state) : Ast.field_ty =
  let nullable = ref false in
  if accept st Token.Question then nullable := true;
  let base_ty = 
    match peek st with
    | Token.Ident "ref" ->
        ignore (advance st);
        Ast.Ref (expect_ident st "ref target type")
    | Token.Ident "multi" ->
        ignore (advance st);
        Ast.Multi (expect_ident st "multi target type")
    | Token.Ident "map" ->
        ignore (advance st);
        expect st Token.Lt "'<'";
        let k = expect_ident st "map key type" in
        expect st Token.Comma "','";
        let v = expect_ident st "map value type" in
        expect st Token.Gt "'>'";
        Ast.Map (k, v)
    | Token.Ident name ->
        ignore (advance st);
        Ast.Scalar name
    | _ -> unexpected st "a field type"
  in
  if !nullable then Ast.Nullable base_ty else base_ty
```

**Parse `?` prefix for return types (line 442):**
```ocaml
let parse_ret_type (st : state) : Ast.field_ty option =
  let nullable = ref false in
  if accept st Token.Question then nullable := true;
  if accept st Token.Arrow then begin
    let t = parse_field_ty st in  (* parse_field_ty now returns field_ty *)
    Some (if !nullable then Ast.Nullable t else t)
  end else None
```

**Parse `?` prefix for parameters:**
```ocaml
let parse_param (st : state) : Ast.param =
  let pos = peek_pos st in
  let conv =
    if accept st Token.KwMut then Ast.Mut
    else if accept st Token.KwTake then Ast.Take
    else Ast.Borrow
  in
  let name = expect_ident st "parameter name" in
  expect st Token.Colon "':'";
  let ty = parse_field_ty st in  (* now returns field_ty *)
  { Ast.id = fresh_id st; pos; name; conv; ty }
```

#### 4. Dump (`dump.ml`) - Render Nullable Types

```ocaml
let rec field_ty_str : Ast.field_ty -> string = function
  | Ast.Scalar s -> s
  | Ast.Ref s -> "ref " ^ s
  | Ast.Multi s -> "multi " ^ s
  | Ast.Map (k, v) -> "map<" ^ k ^ ", " ^ v ^ ">"
  | Ast.Nullable t -> "?" ^ field_ty_str t  (* NEW *)
```

---

### Typechecker Integration (Task 6 - `types.ml`)

When Task 6 creates `types.ml`, it must handle:

1. **Field-kind derivation**:
   - `Nullable t` → `WO_K_NULLABLE` (new .wob kind = 6)
   - Payload kind = `t`'s kind (SCALAR, OWNED, GCREF, TEXT, MULTI, MAP)

2. **Expression typing**:
   - Optional chaining: `x?.field` → requires `x : ?T`
   - Nil checks: `if x != nil then ...` narrows type from `?T` to `T`
   - Null coalescing: `x ?? default` → requires `x : ?T`, `default : T`

3. **Builtin signatures** (update for nullable returns):
   - `map_get` → returns `?V`
   - `fs.stat` → returns `?{size, inode, mtime}`
   - `json.decode` → returns `?T`
   - `env.get` → returns `?Text`
   - `proc.run` → returns `?{code, out, err}`

4. **Type compatibility rules**:
   - `T` → `?T` (implicit upcast)
   - `?T` → `?T` (exact match)
   - `?T` → `T` (requires explicit nil check, WO-E2xx if missing)

---

### .wob Format Changes (Plan 3)

**In `runtime/src/wob.h`:**
```c
enum {
    WO_K_SCALAR = 0,
    WO_K_OWNED = 1,
    WO_K_GCREF = 2,
    WO_K_TEXT = 3,
    WO_K_MULTI = 4,
    WO_K_MAP = 5,
    WO_K_NULLABLE = 6,  // NEW
};
#define WO_K_MAX 6u
```

**Runtime representation:**
- Nullable field = 2 slots: discriminant (uint32_t: 0=null, 1=present) + payload (T's kind)
- For SCALAR payload: 16 bytes total (discriminant + i64)
- For OWNED/GCREF payload: 16 bytes total (discriminant + pointer)
- For TEXT/MULTI/MAP payload: 16 bytes total (discriminant + pointer)

---

## Implementation Tasks

### Phase 1: AST & Parser (Immediate)

- [ ] **Task 1a**: Update `ast.ml`
  - Add `Nullable of field_ty` to `field_ty`
  - Change `param.ty : string` → `field_ty`
  - Change `method_sig.ret : string option` → `field_ty option`
  - Change `method_decl.ret : string option` → `field_ty option`
  - Update `param_conv` documentation

- [ ] **Task 1b**: Update `parser.ml`
  - Modify `parse_field_ty` to handle `?` prefix
  - Modify `parse_ret_type` to use `parse_field_ty` and handle `?`
  - Modify `parse_param` to use `parse_field_ty`
  - Update `parse_sig_head` to handle new return type

- [ ] **Task 1c**: Update `dump.ml`
  - Update `field_ty_str` to handle `Nullable`
  - Update `param_str`, `sig_str`, `dump_method_sig` for new types

### Phase 2: Typechecker (Task 6)

- [ ] **Task 2a**: Create `types.ml` with:
  - Two-pass symbol collection
  - Field-kind derivation including `WO_K_NULLABLE`
  - Expression typing with nullable handling
  - Structural interface satisfaction
  - Self mutability inference

- [ ] **Task 2b**: Add WO-E2xx error codes for nullable violations:
  - WO-E211: Nullable type used without nil check
  - WO-E212: Non-nullable assigned nullable without check
  - WO-E213: Missing nil check before field access on nullable

### Phase 3: Tests

- [ ] **Task 3a**: Add golden fixtures in `test/golden/ast/`:
  - `nullable-field.wo` - `field: ?Text`
  - `nullable-return.wo` - `fn foo() -> ?Int`
  - `nullable-param.wo` - `fn foo(x: ?Int)`
  - `nullable-nested.wo` - `?multi ?Text`, `?map<Int, ?Text>`

- [ ] **Task 3b**: Add must-fail fixtures in `test/golden/types/` (when typechecker exists):
  - Missing nil check
  - Type mismatch with nullable

---

## Migration Notes

### Files to Modify
1. `compiler/src/ast.ml` - Core type definitions
2. `compiler/src/parser.ml` - Parsing logic
3. `compiler/src/dump.ml` - Debug output
4. `compiler/src/types.ml` - New file (Task 6)
5. `compiler/src/dune` - Add `types` module

### Breaking Changes
- `param.ty` changes from `string` to `field_ty`
- `method_sig.ret` changes from `string option` to `field_ty option`
- `method_decl.ret` changes from `string option` to `field_ty option`
- Any code constructing `Ast.param`, `Ast.method_sig`, `Ast.method_decl` directly must be updated

### Compatibility
- Parser changes are backward compatible (existing code without `?` still works)
- AST changes require updating downstream consumers (typechecker, emitter)
- Dump format changes are additive

---

## References

- [Task 6 Plan](2026-08-01-woc-compiler-front.md#task-6-typechecker) - Lines 107-115
- [OOP Compiler VM Design](superpowers/specs/2026-08-01-oop-compiler-vm-design.md) - Section 3, "Nullable types"
- [Systems Track Design](superpowers/specs/2026-08-01-systems-track-design.md) - Part 1, "Null<T> → ?T optional types"
- [.wob Format](plan/oop-vm/00-wob-format.md) - Field kinds