# Diffs

## Abstract Graph Diffs

The problem can be specified as follows.

Consider two directed graphs, containing labelled nodes and edges. Given a
designated root node from each graph, summarise how the reachable subgraphs are
different in a tree-like textual presentation.

We gain considerable flexibility if we split this into two sub problems:
generate a difference *graph* and produce a tree-like textual difference report
from this. The intermediate graph can also be presented as a graph, manipulated,
subject to further analysis, used to produce report variations for different
purposes or perhaps even stored.

There are 3 kinds of node difference and each comparison pair can have any
number of these:

1. node label difference - a purely local change
1. labelled edge added or removed - can be modelled either as a local change or
   as a recursive difference (with a special "absent" node)
1. matching outgoing edge label - recursive difference - a component change

When opening out a difference graph into a difference *tree* it is necessary to
introduce two artificial kinds of difference:

1. already reported - to handle diff sharing
1. being compared - to handle diff cycles
1. list of normal differences - each a
   1. node label difference
   1. edge label difference
   1. matching edge label recursive difference

## Comparison Implementation

Comparison is mostly done pair-wise recursively with a DFS, and using the [SCC
finder](SCC.md).

The current comparison algorithm divides responsibility between `Equals` and
`Compare`. There are also trivial helpers `Added` and `Removed` and top-level
function `CompareSymbols`.

The `Result` type encapsulates the difference between two nodes being compared.
It contains both a list (`Diff`) of differences (`DiffDetail`) and a boolean
quality outcome. The latter is used to propagate inequality information in the
presence of cycles in the diff comparison graph.

Note: With hindsight, the names could have been better; naming is hard.

### `Equals`

`Equals` has the job of computing local differences, matching edges and
obtaining edge differences from `Compare` (or `Added` or `Removed`).

Local differences can easily be rendered as text, but edge differences need
recursive calls to `Compare`. When `Equals` gets the result of a recursive
comparison from `Compare`, it will be in the form which can merged into the
local differences with a helper function.

In general we want each `Equals` to do as little and be as readable as possible
because there are many of them, one per node type. The helper functions should
therefore be chosen for power, laziness and concision.

### `Added` and `Removed`

These take care of

* comparisons where one side is missing

`Compare` could have been overloaded to handle these cases but there are several
reasons not to:

* `Compare` would need to take nullable or optional arguments and the
  absent-absent case would be illegal
* added and removed nodes have none of the other interesting features that
  `Compare` is supposed to handle (and modelling an absent node implies strange
  things for revisited comparisons)
* `Added` and `Removed` don't need to decorate their return values with any
  difference information

### `Compare`

`Compare` has the job of handling some special cases plus the "normal" case of
delegating to `Equals`.

`Compare` has to take care of the following:

* revisited, completed comparisons
* revisited, in-progress comparison
* qualified types
* typedefs
* incomparable types
* comparable types - delegated to `Equals`

Note that the non-trivial special cases relating to typedefs and qualified types
(and their current concrete representations) require non-parallel traversals of
the graphs being compared.

#### Trivial special cases and normal case

The comparison steps are approximately:

1. if comparison already has a known result then return this
1. if comparison already is in progress then return a potential difference
1. start node visit, register the node with the SCC finder
   1. (special cases for qualified types and typedefs)
   1. if nodes are not comparable record a difference
   1. otherwise delegate to `Equals` to determine this
1. finish node visit, updating the outcome held by the SCC finder
1. if an SCC was closed
   1. all equal? discard unwanted potential differences
   1. not all equal? propagate inequality
   1. record all node comparisons as final
1. return result (whether or not tentative)

#### Typedefs

Typedefs are just named type aliases which cannot refer to themselves or later
defined types. The referred-to type is exactly identical to the typedef. So for
difference *finding*, typedefs should just be resolved and skipped over.
However, for *reporting*, it may be still useful to say where a difference came
from. This requires extra handling to collect the typedef names on each side of
the comparison, when there is something to report.

If `Compare` sees a comparison involving a typedef, it resolves typedef chains
on both sides and keeps track of the names. Then it calls itself recursively. If
the result is no-diff, it returns no-diff, otherwise, it reports the differences
between the types at the end of the typedef chains.

An alternative would be to genuinely just follow the epsilons. Store the
typedefs in the diff tree but record the comparison of what they resolve to. The
presentation layer can decorate the comparison text with resolution chains.

Note that qualified typedefs present extra complications.

#### Qualified types

STG currently represents type qualifiers as separate, individual nodes. They are
relevant for finding differences but there may be no guarantee of the order in
which they will appear. For diff reporting, we should always say what qualifiers
have been added or removed but still compare the underlying types.

This implies that when faced with a comparison involving a qualifier, `Compare`
should collect and compare all qualifiers on both sides and treat the types as
compound objects consisting of their qualifiers and the underlying types, either
or both of which may have differences to report. Comparing the underlying types
requires a recursive call to `Compare`.

Note that qualified typedefs present extra complications.

#### Qualified typedefs

Qualifiers and typedefs have subtle interactions. For example:

Before:

```c++
const int quux;
```

After 1:

```c++
typedef int foo;
const foo quux;
```

After 2:

```c++
typedef const int foo;
foo quux;
```

After 3:

```c++
typedef const int foo;
const foo quux;
```

In all cases above, the type of `quux` is unchanged. These examples strongly
suggest that a better model of C types would involve tracking qualification as a
decoration present on every type node, including typedefs.

Note that this behaviour implies C's type system is not purely constructive as
there is machinery to discard duplicate qualifiers which would be illegal
elsewhere.

For the moment, we can pretend that outer qualifications are always significant,
even though they may be absorbed by inner ones, and risk occasional false
positives.

A worse case is:

Before:

```c++
const int quux[];
```

After 1:

```c++
typedef int foo[];
const foo quux;
```

After 2:

```c++
typedef const int foo[];
foo quux;
```

After 3:

```c++
typedef const int foo[];
const foo quux;
```

All the `quux` are identically typed. There is an additional wart that what
would normally be illegal qualifiers on an array type instead decorate its
element type.

Finally, worst is:


Before:

```c++
const int quux();
```

After 1:

```c++
typedef int foo();
const foo quux;
```

After 2:

```c++
typedef const int foo();
foo quux;
```

After 3:

```c++
typedef const int foo();
const foo quux;
```

The two `const foo quux` cases invoke undefined behaviour. The consistently
crazy behaviour would have been to decorate the return type instead.

### Diff helpers

Mainly for `Equals`.

* `AddDiff` - add node/edge label difference, unconditionally
* `MaybeAddDiff` - add matching edge recursive difference, conditionally

Variants are possible where text is generated lazily on a recursive diff being
found, as are ones where labels are compared and serialised only if different.

## Textual Diff Presentation

There are two problems to solve, generating suitable text for nodes, edges and
differences and generating a tree-like report structure.

### Tree structure

This requires a DFS over the difference graph. Nodes which are already seen
should be reported as "being reported" or "already reported" occording to
whether their (first) "visit" is in progress or has already completed.

So each of the cases above can presented in a staightforward fashion.

* type X changed to Y (as already reported)
* type X changed to Y (as being reported)
* type X changed to Y (of a different kind)
* type X changed to Y (of the same kind)
   * they have different sizes
   * member L of type LT was removed
   * member M changed
      * type A changed to B ...

However, the last case may need to be broken down further for STG comparisons,
due to particular C type features that mean edges are not strictly matched and
followed in parallel:

* type X changed to Y (with different qualifiers)
   * qualifiers differ
   * underlying type changed
      * type A changed to B ...
* type X changed to Y (involving typedef resolution)
   * resolved types are different
      * type A changed to B ...

#### Compactifying the presentation

There's no need to indent the presentation twice for matching edge diffs. The
edge description and target comparison description can be folded together:

* type X changed to Y (of the same kind)
  * they have different sizes
  * member L of type LT was removed
  * member M changed: type A changed to B ...

Note: This should be done at *presention time*. It is better not to fold M, A
and B into the same *data structure element* as that would preclude switching
over to producing a diff graph as the A/B comparison could no longer be shared.

#### Can nodes and edges be collaped further?

Another presentation optimisation is possible when dealing with normal diffs
where there will always be exactly one, recursive, diff per node.

* types X * and Y * are different
  * pointed-to type is different: X and Y are ...

This could be:

* types X * and Y * are (via pointed-to type) ...

It's not clear how this would work with chains of pointers (and other similar
things). The loss of information / long lines would also reduce readability.

This approach will likely require heuristics and we'd need to generate extra
text.

## Text elements

* Nodes should have proper descriptions. In particular, we should be able to
  generate full C types with the usual declaration syntax.
* Labelled edge descriptions are things like "member 'foo'" or "pointed-to
  type". It's not clear if declaration syntax would be useful for these too.
* We need to assemble together these pieces when reporting differences.

#### Node naming

Nodes are either symbols (variable or functions) or types. See [Names](NAMES.md)
for detailed notes on C type name syntax.

The generation of names for nodes is deferred to diff presentation.

#### Node label and matching edge label descriptions

Node labels are things like array length, type names etc. Matching edge labels
are things like "pointed to type" and member names.

We need to generate suitable text for all of these. The current difference graph
representation is untyped and we put this information into the graph at the
point of *generation*.

However, we could argue for a typed difference graph, in which case this text
generation would be deferred until *presentation* time.

#### Verb and phrasing choices

Assuming we have good names and descriptions for things, how should we use them
in comparisons, what verbs and what tense should we use?

* X and Y differ?
* X changed to Y?
* X has changed to Y?
* X changed (if descriptions identical)?
* X (if descriptions identical)?
* X -> Y?

To avoid scattering word phrases over a lot of code, it would be useful to have
a helper function that, given two nodes, can build an appropriate phrase. It may
be useful to have a common descriptive prefix. This is for use in the typical,
comparable node case.

* pointer type '...' changed to '...'
  * or, pointer type changed from '...' to '...'
* array type '...' changed

For the incomparable and epsilon transition (typedefs, qualified types) cases,
special coding can be used.

* qualified type '...' changed to unqualified type '...'
* qualified type '...' changed
* via typedef '...'
* then via typedef '...' and now via typedefs '...', '...'
