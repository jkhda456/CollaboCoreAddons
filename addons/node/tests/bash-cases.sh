# bash behaviour cases for the guest port (tools/build-bash.sh): each prints a labelled
# result; tests/bash-compare.sh runs the file with the host's bash and the guest's and
# diffs the output. Nothing here may depend on PIDs, timing or the bash version.
T=${TMPDIR:-/tmp}/bashcases.$$
mkdir -p "$T"; cd "$T"
n=0
c() { n=$((n + 1)); printf '%02d %s: ' "$n" "$1"; }

c pipeline;          echo hello | tr a-z A-Z | rev
c comsub;            echo "[$(echo inner)]"
c comsub-nested;     echo "$(echo a$(echo b$(echo c)))"
c comsub-backtick;   echo `echo tick`
c comsub-status;     x=$(false); echo $?
c comsub-trailing-nl; x=$(printf 'a\n\n\n'); echo "${#x}"
c subshell-noleak;   v=outer; (v=inner; echo "in=$v"); echo "out=$v"
c subshell-cd;       (cd /; pwd); pwd | sed "s|$T|T|"
c subshell-exit;     (exit 7); echo $?
c group;             { echo a; echo b; } | wc -l
c fn-pipeline;       up() { tr a-z A-Z; }; echo shout | up
c fn-recursion;      fact() { if (( $1 <= 1 )); then echo 1; else echo $(( $1 * $(fact $(( $1 - 1 ))) )); fi; }; fact 6
c fn-local;          g=1; f() { local g=2; echo $g; }; f; echo $g
c while-read-pipe;   printf '1\n2\n3\n' | while read l; do echo -n "<$l>"; done; echo
c while-read-noleak; cnt=0; printf 'a\nb\n' | while read l; do cnt=$((cnt+1)); done; echo $cnt
c lastpipe;          shopt -s lastpipe; set +m; cnt=0; printf 'a\nb\n' | while read l; do cnt=$((cnt+1)); done; echo $cnt; shopt -u lastpipe
c heredoc;           cat <<EOF
x=$((1+1))
EOF
c heredoc-quoted;    cat <<'EOF'
$HOME stays
EOF
c heredoc-tab;       cat <<-EOF
	tabbed
	EOF
c herestring;        tr a-z A-Z <<< "here"
c array;             a=(one two three); echo "${a[1]} ${#a[@]} ${a[@]:1}"
c array-append;      a+=(four); echo "${a[-1]} ${#a[@]}"
c array-indices;     b=([3]=x [7]=y); echo "${!b[@]}"
c assoc;             declare -A m=([k1]=v1 [k2]=v2); for k in $(printf '%s\n' "${!m[@]}" | sort); do echo -n "$k=${m[$k]} "; done; echo
c array-in-subshell; a=(x y); (a+=(z); echo ${#a[@]}); echo ${#a[@]}
c regex;             [[ "foo123bar" =~ ([0-9]+) ]] && echo "${BASH_REMATCH[1]}"
c dbl-bracket;       [[ -n x && ( a < b ) ]] && echo yes
c pattern-match;     [[ file.txt == *.txt ]] && echo txt
c arith;             echo $(( (7 + 3) * 2 ** 3 % 11 ))
c arith-cmd;         (( 5 > 3 )) && echo gt
c let;               let "z = 6 * 7"; echo $z
c param-default;     unset u; echo "${u:-def} ${u:=set} $u"
c param-substr;      s=abcdef; echo "${s:2:3} ${s#a*c} ${s%e*} ${s/cd/XY} ${s^^}"
c param-length;      s=abcdef; echo ${#s}
c indirect;          ref=s; echo ${!ref}
c nameref;           declare -n nr=s; nr=changed; echo $s
c brace;             echo {a,b}{1..3}
c brace-seq;         echo {10..1..3}
c glob;              touch g1.txt g2.txt g3.log; echo g*.txt
c glob-nomatch;      echo nomatch*.zz
c nullglob;          shopt -s nullglob; echo "[" nomatch*.zz "]"; shopt -u nullglob
c extglob;           shopt -s extglob; eval 'echo !(*.log)'; shopt -u extglob
c case;              case "apple" in a*) echo A;; *) echo other;; esac
c for-c;             for ((i=0; i<3; i++)); do echo -n $i; done; echo
c until;             i=0; until (( i >= 3 )); do i=$((i+1)); done; echo $i
c break-continue;    for i in 1 2 3 4; do (( i == 2 )) && continue; (( i == 4 )) && break; echo -n $i; done; echo
c and-or;            false && echo no || echo yes
c not;               ! false && echo negated
c exit-status-pipe;  false | true; echo $?
c pipestatus;        true | false | true; echo "${PIPESTATUS[@]}"
c pipefail;          set -o pipefail; false | true; echo $?; set +o pipefail
c set-e;             ( set -e; false; echo not-reached ); echo "status $?"
c set-e-cond;        ( set -e; if false; then :; fi; echo survived )
c set-u;             ( set -u; echo "$undefined_var" ) 2>/dev/null; echo "status $?"
c trap-exit;         ( trap 'echo trapped' EXIT; echo body )
c trap-err;          ( trap 'echo err-trap' ERR; false; true )
c trap-sig;          ( trap 'echo got-usr1' USR1; kill -USR1 $BASHPID; echo after )
c bg-wait;           ( sleep 0.2; exit 3 ) & wait $!; echo "waited $?"
c bg-multi;          for i in 1 2 3; do ( echo job$i > j$i ) & done; wait; cat j1 j2 j3 | tr '\n' ' '; echo
c bg-bang;           sleep 0 & p=$!; [[ $p =~ ^[0-9]+$ ]] && echo pid-ok; wait
c procsub-in;        diff <(echo same) <(echo same) && echo equal
c procsub-out;       echo data > >(tr a-z A-Z > ps.out); wait; sleep 0.1; cat ps.out
c procsub-while;     while read l; do echo "got $l"; done < <(printf 'p\nq\n')
c redirect-out;      echo written > r.txt; cat r.txt
c redirect-append;   echo more >> r.txt; wc -l < r.txt
c redirect-clobber;  set -o noclobber; { echo x > r.txt; } 2>/dev/null || echo blocked; echo forced >| r.txt; cat r.txt; set +o noclobber
c redirect-2to1;     ls /nonexistent-path 2>&1 | grep -c -i "no such"
c redirect-both;     { echo out; echo err >&2; } &> both.txt; sort both.txt | tr '\n' ' '; echo
c exec-fd;           exec 3> fd3.txt; echo via3 >&3; exec 3>&-; cat fd3.txt
c exec-fd-read;      exec 4< fd3.txt; read -u 4 line; exec 4<&-; echo "$line"
c dup-swap;          ( echo to-stderr 1>&2 ) 2>&1 | cat
c source;            echo 'sourced_var=42' > s.sh; source ./s.sh; echo $sourced_var
c eval;              cmd='echo evaluated'; eval "$cmd"
c printf-v;          printf -v out '%05.1f|%x|%s' 3.14159 255 str; echo "$out"
c printf-q;          printf '%q\n' "a b'c"
c declare-p;         declare -a dp=(1 2); declare -p dp
c declare-f;         myfn() { echo hi; }; declare -f myfn | head -1
c declare-i;         declare -i di=5; di+=3; echo $di
c readonly;          ( readonly ro=1; ro=2 ) 2>/dev/null; echo "status $?"
c export-child;      export EXP=exported; sh -c 'echo $EXP'
c env-prefix;        V=prefixed sh -c 'echo $V'; echo "[${V:-}]"
c shopt-list;        shopt -q extglob && echo on || echo off
c alias;             shopt -s expand_aliases; alias hi='echo alias-ran'; eval hi
c getopts;           set -- -a -b val rest; while getopts ab: o; do echo -n "$o${OPTARG:-} "; done; echo
c positional;        set -- x y z; echo "$# $2 ${@: -1}"
c shift;             set -- 1 2 3; shift 2; echo "$@"
c ifs;               IFS=: read -r f1 f2 <<< "a:b"; echo "$f2$f1"
c read-array;        read -ra ra <<< "r s t"; echo ${#ra[@]}
c mapfile;           mapfile -t lines < <(printf 'l1\nl2\n'); echo "${lines[1]}"
c coproc;            coproc CP { read x; echo "co:$x"; }; echo ping >&"${CP[1]}"; read -u "${CP[0]}" ans; echo "$ans"; wait
c sub-of-sub;        ( ( ( echo deep ) ) )
c comsub-in-loop;    s=0; for i in $(seq 1 20); do s=$(( s + $(echo $i) )); done; echo $s
c external-status;   sh -c 'exit 4'; echo $?
c command-not-found; nosuchcommand-xyz 2>/dev/null; echo $?
c type;              type -t echo; type -t cat >/dev/null && echo has-cat
c builtin-time;      { time true; } 2>&1 | grep -c real
c umask-sub;         old=$(umask); ( umask 077; umask ); [[ $(umask) == $old ]] && echo unchanged
c cwd-persist;       cd "$T" && mkdir -p d && cd d && pwd | sed "s|$T|T|"; cd "$T"
c bashpid;           [[ $BASHPID == $$ ]] && echo same; ( [[ $BASHPID != $$ ]] && echo differs )
c subshell-level;    echo $BASH_SUBSHELL $( echo $BASH_SUBSHELL )
c funcname;          fnn() { echo ${FUNCNAME[0]}; }; fnn
c lineno-sub;        echo $(( LINENO > 0 ))
c random-seed;       RANDOM=7; a=$RANDOM; RANDOM=7; b=$RANDOM; [[ $a == $b ]] && echo reproducible
c select-like-case;  x=b; case $x in a|b) echo ab;; esac
c long-pipeline;     seq 1 200 | grep 5 | sort -rn | head -3 | tr '\n' ' '; echo
c big-comsub;        x=$(seq 1 5000); echo ${#x}
c wait-n;            ( sleep 0.1; exit 5 ) & p5=$!; ( exit 6 ) & p6=$!; wait -n $p5 $p6; r=$?; wait; echo $(( r == 5 || r == 6 ))
c kill-bg;           sleep 5 & k=$!; kill $k; wait $k; echo $(( $? > 128 ))
c xtrace;            ( set -x; : traced ) 2>&1 | sed 's/^+* //'

cd /; rm -rf "$T"
echo "done $n"
