# Render the PR Teams card from teams-card.sh state: $p PR, $s statuses, $c check runs, $w workflow runs.
$p[0] as $pr | $s[0] as $st | $c[0] as $cr | $w[0] as $runs |
def jenkins_icon($s; $d; $u):
  if $s == "success" then (if ($d // "" | startswith("Skipped")) then "skipped ⏭" else "passed ✅" end)
  elif $s == "pending" then (if ($u // "") == "" then "queued ⏳" else "running ⏳" end)
  elif $s == "failure" then "failed ❌" else "error ⚠️" end;
def run_icon($r):
  if $r == null then "—"
  elif $r.status != "completed" then "⏳"
  elif $r.conclusion == "success" then "✅"
  elif $r.conclusion == "failure" then "❌"
  elif $r.conclusion == "skipped" then "⏭" else "⚠️" end;
def lbl($l): if any($pr.labels[]; . == $l) then "✅" else "—" end;

($pr.head.ref | [match("RS(DSO|DEV)-[0-9]+").string] | first) as $jira
| (if $pr.merged then "Merged ✔" elif $pr.state == "closed" then "Closed"
   elif $pr.draft then "Draft" else "Ready for review" end) as $state
| [ $st[] | select(.context | test($jre)) ] as $jobs
| [ $cr[] | select(.name | startswith("Aikido")) ] as $aik
| (if ($aik | length) == 0 then null
   elif any($aik[]; .status != "completed") then {t: "running ⏳"}
   elif any($aik[]; .conclusion == "failure") then {t: "failed ❌", bad: true}
   elif any($aik[]; .conclusion == "action_required") then {t: "needs action ⚠️", bad: true}
   elif all($aik[]; .conclusion == "success" or .conclusion == "neutral" or .conclusion == "skipped")
     then {t: "ok ✅"} else {t: "warning ⚠️", bad: true} end
   | if . then . + {url: ([$aik[] | select(.conclusion != "success")][0] // $aik[0]).details_url} else . end
  ) as $aikido
| ($builds | split(",") | map(select(. != ""))) as $names
| [ $names[] as $n
    | ([ $runs[] | select(.name == $n) ] | sort_by(.created_at) | last) as $r
    | {name: ($n | sub("^Build "; "")), icon: run_icon($r), url: ($r.html_url // null),
       bad: ($r != null and $r.status == "completed" and
             ($r.conclusion | IN("success", "skipped", "neutral") | not))} ] as $grid
| ([ ($jobs[] | select(.state == "failure" or .state == "error")
              | {name: .context, url: .target_url}),
     (if $aikido.bad then {name: "Aikido", url: $aikido.url} else empty end),
     ($grid[] | select(.bad) | {name: .name, url: .url}) ] | first) as $fail
| {
  type: "AdaptiveCard", version: "1.4",
  "$schema": "http://adaptivecards.io/schemas/adaptive-card.json",
  msteams: {width: "Full"},
  body: ([
    {type: "TextBlock", size: "Medium", weight: "Bolder", wrap: true,
     text: "PR #\($pr.number) · \($pr.title)"},
    {type: "FactSet", facts: [
      {title: "Author", value: $pr.user.login},
      {title: "Jira", value: (if $jira then "[\($jira)](https://rsjira.realsenseai.com/browse/\($jira))" else "—" end)},
      {title: "Target", value: $pr.base.ref},
      {title: "State", value: $state}]},
    {type: "TextBlock", wrap: true, spacing: "Medium",
     text: "**Agent review:**  CR \(lbl("CR-AGENT-APPROVE"))   CQ \(lbl("CQ-AGENT-APPROVE"))   VC \(lbl("VC-AGENT-APPROVE"))"},
    {type: "FactSet", spacing: "Medium", facts: (
      (if ($jobs | length) == 0 then [{title: $jname, value: "—"}] else [] end)
      + [ $jobs[] | {title: "\(.context)\($queue)",
                   value: (jenkins_icon(.state; .description; .target_url)
                           + (if (.target_url // "") != "" then
                                "  [#\(.target_url | capture("/(?<n>[0-9]+)/?$").n // "build")](\(.target_url))"
                              else "" end))} ]
      + (if $aikido then [{title: "Aikido", value: $aikido.t}] else [] end)
      )}
  ]
  + (if ($grid | length) > 0 then
       [{type: "TextBlock", weight: "Bolder", spacing: "Medium", text: "Builds"}]
       + [ $grid | group_by(.name | capture("JP (?<m>[0-9]+)").m)[]
           | {type: "TextBlock", spacing: "None", wrap: true,
              text: (map("\(.name) \(.icon)") | join("   "))} ]
     else [] end)
  + [{type: "TextBlock", size: "Small", isSubtle: true, spacing: "Medium", text: "Updated: \($updated)"}]),
  actions: (
    [{type: "Action.OpenUrl", title: "Open PR", url: $pr.html_url}]
    + (if $jira then [{type: "Action.OpenUrl", title: "Open Jira",
                       url: "https://rsjira.realsenseai.com/browse/\($jira)"}] else [] end)
    + ([ $jobs[] | select((.target_url // "") != "") ][0]
       | if . then [{type: "Action.OpenUrl", title: "Open build", url: .target_url}] else [] end)
    + (if $fail and ($fail.url // "") != "" then
         [{type: "Action.OpenUrl", title: "Failed: \($fail.name)", url: $fail.url}] else [] end))
}
