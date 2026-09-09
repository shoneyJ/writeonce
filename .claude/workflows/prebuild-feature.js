export const meta = {
  name: 'prebuild-feature',
  description: 'Pre-build research fan-out: ground a feature story, compare references, audit story discipline, produce a go/no-go brief',
  whenToUse: 'Before writing code for a feature/iteration — run the brainstorm-to-ready groundwork as parallel research and get a consolidated pre-build brief',
  phases: [
    { title: 'Understand', detail: 'read the target story + scout relevant .dev/reference projects' },
    { title: 'Analyze', detail: 'one agent per reference project vs the feature concern' },
    { title: 'Audit', detail: 'story-format/frontmatter + dependency-graph/status-board consistency' },
    { title: 'Consolidate', detail: 'settle open forks, fold gaps, go/no-go on readiness' },
  ],
}

/* ---------------------------------------------------------------------------
 * Encodes the ritual this repo follows BEFORE any code lands on a feature:
 *   understand the story  ->  ground the forks in the actual runtime  ->
 *   compare against .dev/reference implementations for gaps  ->  lock the
 *   decisions with KISS defaults  ->  acceptance criteria + deps/status.
 * It does the *parallelizable research* half and hands back a brief; the
 * fork-settling itself stays an interactive brainstorm (human in the loop).
 *
 * Invoke:  Workflow({ name: 'prebuild-feature', args: {
 *            story: 'docs/stories/runtime-v2/09-in-process-tls.md',  // optional
 *            concern: 'outbound TLS client integration',            // optional
 *            references: ['fiber', 'go'] } })                        // optional
 * With no args it locates the current NEXT PLAN target itself.
 * ------------------------------------------------------------------------- */

const story = (args && args.story) || null
const concern = (args && args.concern) || null
const givenRefs = (args && Array.isArray(args.references)) ? args.references : null
const REF_CAP = 6   // keep the fan-out bounded (medium workflow-size guideline)

const UNDERSTAND_SCHEMA = {
  type: 'object',
  properties: {
    storyPath: { type: 'string' },
    concern: { type: 'string' },
    readiness: { type: 'string' },
    lockedDecisions: { type: 'array', items: { type: 'string' } },
    openForks: { type: 'array', items: { type: 'string' } },
    acceptanceCriteria: { type: 'string' },
    outOfScopePresent: { type: 'boolean' },
    summary: { type: 'string' },
  },
  required: ['storyPath', 'concern', 'readiness', 'openForks', 'summary'],
}

const SCOUT_SCHEMA = {
  type: 'object',
  properties: {
    references: { type: 'array', items: { type: 'string' } },
    rationale: { type: 'string' },
  },
  required: ['references'],
}

const REF_SCHEMA = {
  type: 'object',
  properties: {
    project: { type: 'string' },
    howItHandles: { type: 'string' },
    gapsInOurApproach: { type: 'array', items: { type: 'string' } },
    recommendations: { type: 'array', items: { type: 'string' } },
  },
  required: ['project', 'howItHandles'],
}

const AUDIT_SCHEMA = {
  type: 'object',
  properties: {
    area: { type: 'string' },
    ok: { type: 'boolean' },
    issues: { type: 'array', items: { type: 'string' } },
  },
  required: ['area', 'ok', 'issues'],
}

const BRIEF_SCHEMA = {
  type: 'object',
  properties: {
    ready: { type: 'boolean' },
    goNoGo: { type: 'string' },
    unsettledForks: { type: 'array', items: { type: 'string' } },
    recommendedDefaults: { type: 'array', items: { type: 'string' } },
    gapsToFold: { type: 'array', items: { type: 'string' } },
    acceptanceGaps: { type: 'array', items: { type: 'string' } },
    blockers: { type: 'array', items: { type: 'string' } },
    summary: { type: 'string' },
  },
  required: ['ready', 'goNoGo', 'summary'],
}

const CONVENTIONS =
  'Repo discipline: story frontmatter is the ONLY source of status (status + readiness); ' +
  'story docs carry NO code blocks (plans-no-raw-code); brainstorm to readiness:ready with ' +
  'decisions LOCKED and Given/When/Then acceptance criteria + an out-of-scope list before any ' +
  'code lands; docs live under ./docs; the dependency graph is docs/00-dependency-graph.md and ' +
  'the status board docs/stories/00-status.md. Read CLAUDE.md and docs/stories/00-status.md to confirm.'

phase('Understand')

// The target story: use args.story, else let the agent find the NEXT PLAN target.
const storyClause = story
  ? `The target story is ${story}.`
  : 'No story path was given — read docs/stories/00-status.md, find the current in-progress / NEXT-PLAN feature, and use its story file.'
const concernClause = concern ? `The feature concern is: ${concern}.` : 'Infer the feature concern from the story.'

const [understanding, scout] = await parallel([
  () => agent(
    `${storyClause} ${concernClause}\n\n` +
    `Read that story and the repo conventions. ${CONVENTIONS}\n\n` +
    `Report, as data: the resolved story path, the feature concern in one line, the story's ` +
    `readiness, the decisions already LOCKED, the OPEN forks still unsettled, whether ` +
    `Given/When/Then acceptance criteria and an out-of-scope list are present, and a short summary. ` +
    `Do not propose fixes — just report what is and isn't settled.`,
    { label: 'understand-story', phase: 'Understand', agentType: 'general-purpose', schema: UNDERSTAND_SCHEMA },
  ),
  () => agent(
    (givenRefs
      ? `The caller named these reference projects: ${givenRefs.join(', ')}. Confirm each exists under .dev/reference/ and return the ones that do.`
      : `List .dev/reference/ (\`ls .dev/reference\`). ${concernClause} `) +
    `Pick the reference projects most relevant to studying this concern (at most ${REF_CAP}), newest/most-relevant first. ` +
    `Return their directory names and a one-line rationale. Grounded in what actually exists on disk.`,
    { label: 'scout-references', phase: 'Understand', agentType: 'general-purpose', schema: SCOUT_SCHEMA },
  ),
])

const theConcern = (understanding && understanding.concern) || concern || 'the feature concern'
const theStory = (understanding && understanding.storyPath) || story || '(the NEXT-PLAN story)'
let refs = (scout && scout.references) || givenRefs || []
refs = refs.slice(0, REF_CAP)
if (refs.length === 0) log('No reference projects identified — skipping the reference-analysis fan-out.')

// One research batch: a reference-analysis agent per project + two audit agents,
// all independent, all needed by the consolidation barrier.
const research = await parallel([
  ...refs.map((r) => () => agent(
    `Analyze how the reference project .dev/reference/${r} handles "${theConcern}". ` +
    `Read its actual source (grep/read the relevant files). Report: how it handles the concern; ` +
    `where writeonce's planned approach in ${theStory} has GAPS or missing safeguards versus it; ` +
    `and concrete recommendations. Be specific and cite files. Return raw data, not prose for a human.`,
    { label: `ref:${r}`, phase: 'Analyze', agentType: 'general-purpose', schema: REF_SCHEMA },
  )),
  () => agent(
    `Audit ${theStory} against the repo's STORY DISCIPLINE. ${CONVENTIONS}\n` +
    `Check: frontmatter carries status + readiness; NO code fences in the doc; decisions are LOCKED ` +
    `(not vague); Given/When/Then acceptance criteria present; out-of-scope list present. ` +
    `Report each violation as an issue; ok=true only if clean.`,
    { label: 'audit:story-format', phase: 'Audit', agentType: 'general-purpose', schema: AUDIT_SCHEMA },
  ),
  () => agent(
    `Audit consistency between ${theStory}, the dependency graph (docs/00-dependency-graph.md) and the ` +
    `status board (docs/stories/00-status.md) for "${theConcern}". Check: the feature's node/row exists, ` +
    `its status matches the story frontmatter, and blockers/dependencies named in the story appear in the ` +
    `graph. Report mismatches as issues; ok=true only if consistent.`,
    { label: 'audit:deps-status', phase: 'Audit', agentType: 'general-purpose', schema: AUDIT_SCHEMA },
  ),
])

const refResults = research.slice(0, refs.length).filter(Boolean)
const audits = research.slice(refs.length).filter(Boolean)

phase('Consolidate')

const brief = await agent(
  `You are consolidating a PRE-BUILD brief for "${theConcern}" (story ${theStory}) — the go/no-go before code.\n\n` +
  `Understanding of the story:\n${JSON.stringify(understanding, null, 2)}\n\n` +
  `Reference analyses (gaps vs our approach):\n${JSON.stringify(refResults, null, 2)}\n\n` +
  `Story-discipline + deps/status audits:\n${JSON.stringify(audits, null, 2)}\n\n` +
  `Produce the brief: the OPEN forks still to settle (each with a recommended KISS default); ` +
  `the gaps from the reference analyses worth FOLDING IN as locked requirements before build; ` +
  `any acceptance-criteria gaps; blockers; and a clear go/no-go on whether the story is truly ` +
  `ready to build. ready=true only if the forks are settled, the audits are clean, and the ` +
  `reference gaps are either folded in or explicitly deferred. Ground every point in the inputs above.`,
  { label: 'consolidate-brief', phase: 'Consolidate', effort: 'high', schema: BRIEF_SCHEMA },
)

log(`Pre-build brief for ${theConcern}: ${brief && brief.goNoGo ? brief.goNoGo : '(no verdict)'}`)

return { story: theStory, concern: theConcern, understanding, references: refResults, audits, brief }
