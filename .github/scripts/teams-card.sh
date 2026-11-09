#!/usr/bin/env bash
# Build the PR's Teams card from live GitHub state; the flow posts it once, then edits it in place.
# Env: REPO GH_TOKEN PR JENKINS_RE JENKINS_JOB [BUILDS QUEUE_SHA NOTIFY_TEXT (adds a pinging reply)].
# Without HOOK it prints the payload (dry run). SETTLE=<s> re-reads after that delay and re-posts if changed.
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)

fetch() {
  local prj sha
  prj=$(gh api "repos/$REPO/pulls/$PR")
  if [ "$(jq -r '.draft' <<<"$prj")" = true ] && [ -z "${QUEUE_SHA:-}" ]; then
    echo "PR $PR is a draft"; exit 0
  fi
  sha=$(jq -r '.head.sha' <<<"$prj")
  gh api graphql -F n="$PR" -f o="${REPO%/*}" -f r="${REPO#*/}" -F query=@"$DIR/teams-card.graphql" \
    --jq '.data.repository.pullRequest' > "$T/gql" || echo '{}' > "$T/gql"
  # Human review leaves out bots and the author (agent reviewers post as the author).
  jq --slurpfile g "$T/gql" '.user.login as $a | $g[0] as $q | ($q.latestOpinionatedReviews.nodes // []
      | map(select(.author.__typename == "User" and .author.login != $a))) as $rv
    | {number, title, html_url, draft, state, merged, queued: ($q.mergeQueueEntry != null),
       review: {decision: $q.reviewDecision,
                approvers: [$rv[] | select(.state == "APPROVED") | .author.login],
                blockers: [$rv[] | select(.state == "CHANGES_REQUESTED") | .author.login],
                requested: [$q.reviewRequests.nodes[]?.requestedReviewer | (.login // .name) | values]},
       labels: [.labels[].name], user: {login: .user.login}, head: {ref: .head.ref}, base: {ref: .base.ref}}' \
    <<<"$prj" > "$T/pr"
  # A merge-queue run tests its own commit, so its Jenkins row comes from that commit.
  gh api "repos/$REPO/commits/${QUEUE_SHA:-$sha}/status" \
    --jq '[.statuses[] | {context, state, description, target_url}]' > "$T/st" || echo '[]' > "$T/st"
  gh api "repos/$REPO/commits/$sha/check-runs?per_page=100" \
    --jq '[.check_runs[] | {name, status, conclusion, details_url}]' > "$T/cr" || echo '[]' > "$T/cr"
  echo '[]' > "$T/runs"
  if [ -n "${BUILDS:-}" ]; then
    gh api "repos/$REPO/actions/runs?head_sha=$sha&per_page=100" \
      --jq '[.workflow_runs[] | {name, status, conclusion, html_url, created_at}]' > "$T/runs" \
      || echo '[]' > "$T/runs"
  fi
}

build() {
  local card
  fetch
  card=$(jq -n -f "$DIR/teams-card.jq" \
    --slurpfile p "$T/pr" --slurpfile s "$T/st" --slurpfile c "$T/cr" --slurpfile w "$T/runs" \
    --arg jre "$JENKINS_RE" --arg jname "$JENKINS_JOB" --arg builds "${BUILDS:-}" \
    --arg queue "${QUEUE_SHA:+ (merge queue)}" --arg updated "$(TZ=Asia/Jerusalem date '+%Y-%m-%d %H:%M')")
  PAYLOAD=$(jq -n --arg key "PR-$PR" --argjson card "$card" --arg notify "${NOTIFY_TEXT:-}" \
    --slurpfile p "$T/pr" \
    '$p[0] as $pr | {key: $key, mode: "card", card: $card, notify: ($notify != ""), notifyText: $notify,
      title: $pr.title, author: $pr.user.login, url: $pr.html_url, status: "updated", source: "card"}')
}

sig() { jq -c '.card | del(.body[] | select((.text? // "") | startswith("Updated:")))' <<<"$PAYLOAD"; }

build
if [ -z "${HOOK:-}" ]; then echo "$PAYLOAD"; exit 0; fi
curl -sf -X POST "$HOOK" -H 'Content-Type: application/json' -d "$PAYLOAD"
# Parallel jobs post snapshots out of order; a late re-read leaves the newest state last.
[ "${SETTLE:-0}" -gt 0 ] || exit 0
SENT=$(sig); sleep "$SETTLE"; NOTIFY_TEXT=""; build
[ "$(sig)" = "$SENT" ] || curl -sf -X POST "$HOOK" -H 'Content-Type: application/json' -d "$PAYLOAD"
