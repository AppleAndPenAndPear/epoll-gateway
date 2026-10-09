# P2: after the verified baseline, validate through issues and public discussion

> This note is a project-planning draft, not a feature claim. The working assumption is: once the release package and the local correctness baseline are green, the project should move from interview-first validation to issue-driven external discussion and a public technical article. The goal is to gather signal without delaying the core engineering work.

## Why start P2 this way

The current project baseline is now much clearer than it was during the first round of design work:

- the repo has a stable release artifact and distribution-level verification
- the core CI and integration assertions are green
- the benchmark path has been corrected and the results are documented
- the main known limitations are now explicit, especially around async upstream fan-out and composition

That means the project is no longer in a vague exploration phase. It is in a phase where the next question is not "can we build something that works in isolation?" but rather "what are the right trade-offs to carry into the next layer of functionality?"

The answer is to run P2 in a more externally grounded way:

- do not block the next engineering round on formal interviews
- do not claim broad market validation before the foundation is mature
- instead, study relevant open issues in adjacent projects, compare design constraints, and turn the result into a public technical note

## Operating model for P2

### 1. Keep the engineering workload narrow

P2 should focus on the bounded asynchronous upstream foundation:

- no blocking epoll workers while waiting for upstreams
- explicit queue / concurrency / memory bounds
- deadlines and cancellation semantics
- overload and shutdown behavior
- slow-backend failure and recovery tests

The core output is concrete behavior, not a broad feature claim.

### 2. Choose a small set of external issue threads

The idea is not to do a full market study. It is to pick 2-3 issue discussions in adjacent projects that touch the same class of problems:

- request cancellation under slow upstreams
- backpressure and queue design
- partial failure semantics in API composition
- timeout policy for upstream fan-out
- graceful shutdown while requests are in flight

From those threads, we gather practical questions such as:

- what are the failure modes people are actually hitting?
- what is the default timeout model people expect?
- how much partial-result behavior is considered acceptable?
- what documentation do reviewers expect before they trust a gateway with fan-out?

### 3. Publish the discussion as a technical article

The output should be a short article or note that explains:

- what was validated locally
- what remains intentionally out of scope
- what the project is choosing to do in P2
- why the async model is bounded rather than a broad orchestration framework
- what concrete issue discussions influenced the design

The article should invite comments from people who have their own failure stories, rather than presenting the project as a finished solution.

## Why this is better than interview-first work

Interview-first validation is useful, but at this stage it often produces a large set of unprioritized requirements. That is not the right signal for a small gateway project at this maturity level.

Issue-driven discussion gives a cleaner signal:

- it keeps the work grounded in real-world failure modes
- it avoids inventing large product requirements too early
- it gives the project concrete public feedback before the next round of product interviews
- it creates an article trail that can later be used in a more official launch or community post

## Concrete next actions

1. Complete the P2 async upstream design and tests for bounded concurrency and shutdown.
2. Identify a small set of related open-source issues in adjacent projects.
3. Write a short article summarizing the design trade-offs and what the issue discussions taught us.
4. Post the article in community channels and gather comments.
5. Revisit user interviews only after the base feature set is more mature and the project is ready for a sharper product direction.

This path keeps the project honest: we validate the product and engineering decisions while remaining open to public critique, without pretending the user interviews have already happened when the foundation is still being tightened.
