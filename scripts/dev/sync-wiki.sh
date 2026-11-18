#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Publish the documentation under docs/ to the repository GitHub wiki.
#
# One wiki page is generated for each docs/*.md, named after the file stem, and
# Home.md indexes every page by title and first sentence. Each generated page
# starts with a banner naming the repository file it comes from, so the
# repository stays the only source of truth and the pages are never edited on
# the wiki.
#
#   scripts/dev/sync-wiki.sh
#       Clone the wiki repository into a temporary directory, write the pages,
#       commit only when something changed and push.
#   scripts/dev/sync-wiki.sh --check [--wiki-dir DIR]
#       Generate the pages twice in a temporary directory, compare the runs and
#       report drift against a local wiki checkout. Never uses the network.
#
# Environment:
#   REFRAIN_WIKI_TOKEN   user access token that may push the wiki repository
#                        (WIKI_TOKEN is accepted as a fallback); without one
#                        git uses its configured credential helper.
#   REFRAIN_WIKI_REMOTE  wiki git remote, defaults to the project wiki.
#   REFRAIN_REPO_URL     repository address used in the page banner.
#   REFRAIN_WIKI_DIR     wiki checkout compared by --check and updated by a
#                        sync when --wiki-dir is not passed.
#
# The wiki is a separate git repository. The GITHUB_TOKEN a workflow receives is
# scoped to the source repository and cannot push it, so an unattended sync
# needs a user access token or an equivalent credential stored as a secret.

set -euo pipefail

# Byte-wise comparison and sorting matter: the output must not depend on the
# machine locale.
export LC_ALL=C

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DOCS="$ROOT/docs"

DEFAULT_REMOTE="https://github.com/OtonashiHorobu/refrain.wiki.git"
DEFAULT_REPO_URL="https://github.com/OtonashiHorobu/refrain"

REMOTE="${REFRAIN_WIKI_REMOTE:-$DEFAULT_REMOTE}"
REPO_URL="${REFRAIN_REPO_URL:-$DEFAULT_REPO_URL}"
TOKEN="${REFRAIN_WIKI_TOKEN:-${WIKI_TOKEN:-}}"
WIKI_DIR="${REFRAIN_WIKI_DIR:-}"
MODE=sync
KEEP=0
WORK=""

say() { printf '%s\n' "$*"; }
die() { printf 'sync-wiki: %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<'EOF'
usage: scripts/dev/sync-wiki.sh [--check] [--wiki-dir DIR] [--keep]

  --check         generate the pages into a temporary directory and report
                  drift against a wiki checkout; never uses the network
  --wiki-dir DIR  wiki checkout to update (sync, local page edits are
                  discarded) or to compare with (--check)
  --keep          keep the temporary directory and print its path
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --check) MODE=check ;;
        --wiki-dir)
            [ $# -ge 2 ] || die "--wiki-dir needs a directory"
            shift
            WIKI_DIR="$1"
            ;;
        --keep) KEEP=1 ;;
        -h|--help) usage; exit 0 ;;
        *)
            printf 'sync-wiki: unknown argument: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

cleanup() {
    if [ -n "$WORK" ]; then
        if [ "$KEEP" -eq 1 ]; then
            printf 'sync-wiki: kept work directory: %s\n' "$WORK" >&2
        else
            rm -rf "$WORK"
        fi
    fi
}
trap cleanup EXIT

for tool in awk basename cat cmp grep head mktemp sed tr wc; do
    command -v "$tool" >/dev/null 2>&1 || die "required tool not found: $tool"
done
if [ "$MODE" = sync ]; then
    command -v git >/dev/null 2>&1 || die "required tool not found: git"
    if [ -n "$TOKEN" ]; then
        command -v base64 >/dev/null 2>&1 || die "required tool not found: base64"
    fi
fi
[ -d "$DOCS" ] || die "documentation directory not found: $DOCS"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/refrain-wiki.XXXXXX")"

# Page stems in byte order, one per line.
PAGES="$WORK/pages"
EXPECTED="$WORK/expected"
: > "$PAGES"
for src in "$DOCS"/*.md; do
    [ -f "$src" ] || die "no documentation pages found under $DOCS"
    basename "$src" .md >> "$PAGES"
done
sed 's/$/\.md/' "$PAGES" > "$EXPECTED"
printf 'Home.md\n' >> "$EXPECTED"

# One sed program rewrites repository links on the wiki pages. Links to
# ../<path> become repository URLs; links to a sibling <stem>.md become links
# to the wiki page <stem> and keep any #fragment. Fenced code is marked by the
# caller so the program never edits examples.
LINK_SED="$WORK/links.sed"
{
    printf '/^@@FENCE@@/!s#](\\.\\./\\([^)]*\\))#](%s/\\1)#g\n' "$REPO_URL"
    while IFS= read -r stem; do
        printf '/^@@FENCE@@/!s#](%s\\.md#](%s#g\n' "$stem" "$stem"
    done < "$PAGES"
} > "$LINK_SED"

doc_banner() {
    local rel="$1"
    # Markdown code spans in the banner are literal, not shell expansions.
    # shellcheck disable=SC2016
    printf '> This page is generated from the repository file [`%s`](%s/blob/main/%s), which is the source of truth. Edit that file and run `scripts/dev/sync-wiki.sh` to update this page.' \
        "$rel" "$REPO_URL" "$rel"
}

home_banner() {
    # shellcheck disable=SC2016
    printf '> This page is generated from the repository [`docs/` tree](%s/tree/main/docs), which is the source of truth. Edit the repository files and run `scripts/dev/sync-wiki.sh` to update this page.' \
        "$REPO_URL"
}

rewrite_links() {
    awk '
        /^[[:space:]]*```/ { fence = !fence; print "@@FENCE@@" $0; next }
        fence { print "@@FENCE@@" $0; next }
        { print }
    ' "$1" | sed -f "$LINK_SED" | sed 's/^@@FENCE@@//'
}

emit_page() {
    local src="$1" banner="$2" out="$3" plain
    plain="$WORK/page.plain"
    {
        printf '%s\n\n' "$banner"
        cat "$src"
    } > "$plain"
    rewrite_links "$plain" > "$out"
}

page_title() {
    awk '
        /^[[:space:]]*```/ { fence = !fence; next }
        fence { next }
        /^# / { sub(/^# /, ""); print; exit }
    ' "$1"
}

# The first sentence of the first prose paragraph below the H1. Headings,
# tables, fenced code, lists, quotes and link-only lines do not count, so a
# page that opens with a table still contributes a sentence to the index.
page_summary() {
    awk '
        function structural(line) {
            if (line == "") return 1
            if (line ~ /^[[:space:]]*(#|```)/) return 1
            if (line ~ /^[[:space:]]*\|/) return 1
            if (line ~ /^[[:space:]]*>/) return 1
            if (line ~ /^[[:space:]]*([-*+] |[0-9]+\. )/) return 1
            if (line ~ /^[[:space:]]*<!--/) return 1
            if (line ~ /^[[:space:]]*---[[:space:]]*$/) return 1
            if (line ~ /^\[[^]]*\](\([^)]*\))$/) return 1
            return 0
        }
        {
            if (!started) {
                if ($0 ~ /^# /) started = 1
                next
            }
            if (done) next
            if (!in_para) {
                if (structural($0)) next
                para = $0
                in_para = 1
                next
            }
            if (structural($0)) { done = 1; next }
            para = para " " $0
        }
        END {
            if (para == "") exit
            gsub(/[[:space:]]+/, " ", para)
            sub(/^ /, "", para)
            sub(/ $/, "", para)
            n = 0
            i = index(para, ". ")
            if (i > 0) n = i
            i = index(para, "! ")
            if (i > 0 && (n == 0 || i < n)) n = i
            i = index(para, "? ")
            if (i > 0 && (n == 0 || i < n)) n = i
            if (n > 0) para = substr(para, 1, n)
            sub(/:$/, "", para)
            if (para !~ /[.!?]$/) para = para "."
            print para
        }
    ' "$1"
}

write_home() {
    local out="$1" plain stem src title summary
    plain="$WORK/home.plain"
    {
        printf '# Refrain documentation\n\n'
        home_banner
        printf '\n\n'
        while IFS= read -r stem; do
            src="$DOCS/$stem.md"
            title="$(page_title "$src")"
            [ -n "$title" ] || title="$stem"
            summary="$(page_summary "$src")"
            if [ -n "$summary" ]; then
                printf -- '- [%s](%s) — %s\n' "$title" "$stem" "$summary"
            else
                printf -- '- [%s](%s)\n' "$title" "$stem"
            fi
        done < "$PAGES"
    } > "$plain"
    rewrite_links "$plain" > "$out"
}

remove_stale_pages() {
    local dest="$1" f name
    for f in "$dest"/*.md; do
        [ -e "$f" ] || continue
        name="$(basename "$f")"
        grep -qxF "$name" "$EXPECTED" && continue
        # Only pages this script generated are removed; a hand-written page
        # such as _Sidebar.md stays where the operator put it.
        if head -n 1 "$f" | grep -q '^> This page is generated from the repository'; then
            rm -f "$f"
            printf 'sync-wiki: removed stale page: %s\n' "$name"
        fi
    done
}

write_pages() {
    local dest="$1" stem src
    mkdir -p "$dest"
    while IFS= read -r stem; do
        src="$DOCS/$stem.md"
        emit_page "$src" "$(doc_banner "docs/$stem.md")" "$dest/$stem.md"
    done < "$PAGES"
    write_home "$dest/Home.md"
    remove_stale_pages "$dest"
}

# Compare the generated tree with a checkout; print the drift and return 1.
compare_checkout() {
    local generated="$1" checkout="$2" drift=0 stem f name
    while IFS= read -r stem; do
        name="$stem.md"
        if [ ! -f "$checkout/$name" ]; then
            printf 'missing: %s\n' "$name"
            drift=1
        elif ! cmp -s "$generated/$name" "$checkout/$name"; then
            printf 'drift: %s\n' "$name"
            drift=1
        fi
    done < "$PAGES"
    if [ ! -f "$checkout/Home.md" ]; then
        printf 'missing: Home.md\n'
        drift=1
    elif ! cmp -s "$generated/Home.md" "$checkout/Home.md"; then
        printf 'drift: Home.md\n'
        drift=1
    fi
    for f in "$checkout"/*.md; do
        [ -e "$f" ] || continue
        name="$(basename "$f")"
        grep -qxF "$name" "$EXPECTED" && continue
        if head -n 1 "$f" | grep -q '^> This page is generated from the repository'; then
            printf 'stale: %s\n' "$name"
            drift=1
        fi
    done
    return "$drift"
}

compare_determinism() {
    local first="$1" second="$2" name bad=0
    while IFS= read -r name; do
        if ! cmp -s "$first/$name" "$second/$name"; then
            printf 'nondeterministic: %s\n' "$name" >&2
            bad=1
        fi
    done < "$EXPECTED"
    return "$bad"
}

page_count() {
    wc -l < "$EXPECTED" | tr -d ' '
}

run_check() {
    write_pages "$WORK/first"
    write_pages "$WORK/second"
    compare_determinism "$WORK/first" "$WORK/second" ||
        die "generated pages differ between two runs"

    # A checkout passed explicitly or through the environment is compared
    # byte for byte. Without one nothing can be compared offline, so the
    # generation itself is the check.
    say "pages: $(page_count)"
    while IFS= read -r stem; do
        say "  $stem.md < docs/$stem.md"
    done < "$PAGES"
    say "  Home.md < index of the pages above"
    say "deterministic: two generations are identical"

    if [ -n "$WIKI_DIR" ]; then
        [ -d "$WIKI_DIR" ] || die "wiki checkout not found: $WIKI_DIR"
        if compare_checkout "$WORK/first" "$WIKI_DIR"; then
            say "wiki checkout is up to date: $WIKI_DIR"
        else
            say "wiki checkout is behind docs/: $WIKI_DIR" >&2
            exit 1
        fi
    else
        say "no wiki checkout compared: pass --wiki-dir or set REFRAIN_WIKI_DIR"
    fi
}

wiki_auth_env() {
    local count encoded
    [ -n "$TOKEN" ] || return 0
    count="${GIT_CONFIG_COUNT:-0}"
    encoded="$(printf 'x-access-token:%s' "$TOKEN" | base64 | tr -d '\n')"
    export "GIT_CONFIG_KEY_$count=http.extraHeader"
    export "GIT_CONFIG_VALUE_$count=AUTHORIZATION: basic $encoded"
    export GIT_CONFIG_COUNT=$((count + 1))
}

clone_error() {
    local err
    err="$(cat "$WORK/clone.err")"
    if printf '%s' "$err" | grep -qi 'not found\|does not exist'; then
        die "wiki repository not found: $REMOTE. Create the first wiki page once in the GitHub web interface, then run this script again"
    fi
    if printf '%s' "$err" | grep -qi 'authentication\|permission denied\|could not read\|terminal prompts disabled'; then
        die "cannot read $REMOTE. Set REFRAIN_WIKI_TOKEN to a token that may write the wiki repository, or configure a git credential helper"
    fi
    if printf '%s' "$err" | grep -qi 'could not resolve host\|network is unreachable\|connection timed out\|connection refused'; then
        die "cannot reach $REMOTE. Check the network and any proxy configuration"
    fi
    printf '%s\n' "$err" >&2
    die "failed to clone $REMOTE"
}

push_error() {
    local err
    err="$(cat "$WORK/push.err")"
    if printf '%s' "$err" | grep -qi 'authentication\|permission denied\|could not read\|terminal prompts disabled'; then
        die "cannot push $REMOTE. Set REFRAIN_WIKI_TOKEN to a token that may write the wiki repository, or configure a git credential helper"
    fi
    printf '%s\n' "$err" >&2
    die "failed to push to $REMOTE"
}

run_sync() {
    wiki_auth_env
    export GIT_TERMINAL_PROMPT=0

    if [ -n "$WIKI_DIR" ]; then
        [ -d "$WIKI_DIR" ] || die "wiki checkout not found: $WIKI_DIR"
        [ -d "$WIKI_DIR/.git" ] || die "not a git checkout: $WIKI_DIR"
        say "updating wiki checkout: $WIKI_DIR"
        git -C "$WIKI_DIR" fetch --quiet origin 2>"$WORK/clone.err" || clone_error
        branch="$(git -C "$WIKI_DIR" symbolic-ref --short HEAD 2>/dev/null || true)"
        [ -n "$branch" ] || die "cannot determine the wiki branch in $WIKI_DIR"
        git -C "$WIKI_DIR" reset --hard --quiet "origin/$branch"
    else
        say "cloning $REMOTE"
        git clone --depth 1 --quiet "$REMOTE" "$WORK/wiki" 2>"$WORK/clone.err" || clone_error
        WIKI_DIR="$WORK/wiki"
        branch="$(git -C "$WIKI_DIR" symbolic-ref --short HEAD 2>/dev/null || true)"
        [ -n "$branch" ] || die "cannot determine the wiki branch in $WIKI_DIR"
    fi

    write_pages "$WIKI_DIR"
    git -C "$WIKI_DIR" add -A
    if git -C "$WIKI_DIR" diff --cached --quiet; then
        say "wiki is up to date: $REMOTE"
        return 0
    fi

    if [ -z "$(git -C "$WIKI_DIR" config user.name || true)" ] ||
        [ -z "$(git -C "$WIKI_DIR" config user.email || true)" ]; then
        die "git needs user.name and user.email to commit the wiki pages"
    fi

    git -C "$WIKI_DIR" commit --quiet -m "docs(wiki): sync pages from docs/"
    say "pushing to $REMOTE"
    git -C "$WIKI_DIR" push --quiet origin "HEAD:refs/heads/$branch" 2>"$WORK/push.err" ||
        push_error
    say "published $(page_count) pages: ${REMOTE%.git}"
}

case "$MODE" in
    check) run_check ;;
    sync) run_sync ;;
esac
