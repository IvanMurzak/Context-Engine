# Query language (R-CLI-012)

The **one specified query language** of the Context Engine query surface (R-CLI-006), part of the
versioned public contract (R-CLI-004). The same predicate/projection language is used across all
three query surfaces — the **derived world**, **live-sim state**, and **schema introspection** — so
an agent reuses one grammar everywhere; there is deliberately **not** a per-surface dialect.

This document is the human-facing companion to the machine description emitted by
`context describe --json` under `contract.queryLanguage`. Both are generated from the ONE module
`src/editor/contract/query_language.{h,cpp}`, and the conformance test
(`src/editor/contract/tests/test_query_language.cpp`) locks the published grammar to the parser the
engine actually runs.

> **Stability.** The contract is UNSTABLE while `protocolMajor == 0` (it may change without a
> deprecation cycle until the M3 freeze). Everything below is additive-only until then. This module
> is the language **specification** + parser/AST + comparators + cursor codec; *executing* a parsed
> query over a live derived world is the `(query, executor)` split (R-LANG-009), which lands with the
> ECS / spatial index it runs against.

## Grammar (EBNF)

Predicate expressions (the "where" clause) and the order-by clause, in ISO/IEC 14977 EBNF:

```ebnf
query        = disjunction ;
disjunction  = conjunction , { "or" , conjunction } ;
conjunction  = negation , { "and" , negation } ;
negation     = [ "not" ] , primary ;
primary      = "(" , disjunction , ")" | predicate ;
predicate    = existence | string-match | comparison ;
existence    = "has" , "(" , field , ")" ;
string-match = str-fn , "(" , field , "," , string , ")" ;
str-fn       = "contains" | "startswith" | "endswith" | "matches"
             | "icontains" | "istartswith" | "iendswith" | "imatches" ;
comparison   = field , cmp-op , literal ;
cmp-op       = "==" | "!=" | "<=" | ">=" | "<" | ">" ;
field        = ident , { ("." , ident) | ("[" , index , "]") } ;
literal      = number | string | "true" | "false" | "null" ;
ident        = ( letter | "_" ) , { letter | digit | "_" | "-" } ;
string       = '"' , { char | escape } , '"' ;
escape       = "\" , ( '"' | "\" | "n" | "t" ) ;
number       = [ "-" ] , digit , { digit } , [ "." , { digit } ] ;
index        = digit , { digit } ;
order-by     = order-term , { "," , order-term } ;
order-term   = ( field | "@id" ) , [ "asc" | "desc" ] ;
```

An empty predicate matches everything (the identity). `and` binds tighter than `or`; `not` binds
tightest; parentheses override precedence. Bare `and` / `or` / `not` are keywords only when not part
of a longer identifier (so `andy == 1` compares the field `andy`).

## Enumerated operator set

| Class          | Operators                                        | Notes |
| -------------- | ------------------------------------------------ | ----- |
| Equality       | `==` `!=`                                         | scalar equality over number / string / bool / null |
| Range          | `<` `<=` `>` `>=`                                 | numbers compare numerically; strings by NFC code-point order |
| Existence      | `has(field)`                                      | the field path resolves to a present value |
| String-match   | `contains` `startswith` `endswith` `matches`      | RHS is a string literal; `matches` is a `*`/`?` glob |

Every string-match operator has a case-**insensitive** `i`-prefixed form
(`icontains` / `istartswith` / `iendswith` / `imatches`). Boolean composition — `and`, `or`, `not` —
is structural in the AST, not a comparison operator.

## Mandatory total ordering

Results carry a **mandatory total order**. The sort always ends with an implicit `@id asc`
tiebreaker (the entity id `@id` is unique), appended automatically when the caller did not already
sort by `@id`. Because the order is total, two distinct rows can never compare equal, so **a
paginated sequence never reorders across pages**. Missing keys sort **last** (a defined, stable
rule) regardless of the term's direction. The default order — when no `order-by` is given — is
`@id asc`.

## Unified cursor (R-CLI-012 unified with R-BRIDGE-008)

Pagination uses **one cursor model**, shared with the event stream — never a second cursor shape.

```
context-cur://v0/<incarnationId>/<generation>/<seq>?after=<hex(entityId)>
```

| Field           | Meaning |
| --------------- | ------- |
| `incarnationId` | the daemon incarnation; a restart invalidates the cursor (forces a fresh snapshot). Shared verbatim with R-BRIDGE-008. |
| `seq`           | the R-BRIDGE-008 monotonic, totally-ordered event `seq` — the event catch-up position. |
| `generation`    | the derived-world generation this page is consistent to (every page of one paged query reads a single generation). |
| `after`         | keyset position: the last entity id returned under the total order, hex-encoded. Empty for an event cursor. |

An **event cursor is exactly a query cursor with an empty `after`**. Query paging adds `generation`
+ `after` for mutation-stable **keyset** pagination: the next page starts strictly after the last
returned id, so concurrent inserts/deletes never shift the already-returned window. This mirrors the
R-CLI-017 large-result contract — a *handle* carries one oversized payload, a *cursor* a
paged/streamed sequence. Treat the token as opaque and feed it back verbatim.

## String semantics

- **Normalization: NFC.** Comparisons operate on NFC-normalized values so canonically-equivalent
  spellings compare equal.
- **Case handling is explicit.** Comparisons are case-**sensitive** by default; the `i`-prefixed
  string-match forms are case-**insensitive**.
- **v1 scope (honest).** Every string literal is strictly UTF-8 validated. ASCII is invariant under
  NFC and passes through; ASCII case-folding backs the `i`-forms. The full Unicode canonical
  decomposition+composition table and non-ASCII case-folding are the tracked follow-up — the
  *semantics* are contract now, so activating the full table later is non-breaking.

## Composability

Queries compose with mutation verbs at the shell: `context query … | context set …`. The query
result's entity ids + file/JSON-pointer provenance feed the following mutation (R-CLI-006).

## Diagnostics

Parse and pagination failures draw from the R-CLI-008 error catalog (usage exit class 2):
`query.syntax_error`, `query.unknown_operator`, `query.invalid_cursor`, `query.unsupported_surface`.
A malformed query is never a best-effort partial parse — it fails with a code and a byte offset.
