#!/bin/sh
# Runs in the guest (tests/run.sh boots it): every step's output in $OUT/NAME.{out,err,code}.
T=/t
OUT=$T/out
rm -rf $OUT; mkdir -p $OUT
cd $T/site && python3 -m http.server 8000 --bind 127.0.0.1 >/dev/null 2>&1 &
sleep 2
step() { name=$1; shift; ( "$@" ) > $OUT/$name.out 2> $OUT/$name.err; echo $? > $OUT/$name.code; }
export WEB_START_TIMEOUT=600
step open        web open http://127.0.0.1:8000/index.html 180
step info        web info
step read        web read
step eval        web eval 'document.title + " " + (1 + 2)'
step korean      web eval 'document.querySelector("h1").textContent'
step type        web type 'Claude 한글' '#name'
step select      web select '#color' green
step submit      web click Greet
step result      web eval 'document.getElementById("out").textContent'
step count1      web click Count
step count2      web click Count
step counted     web eval 'document.getElementById("count").textContent'
step keys        web key Tab shift+Tab ctrl+a
step keyseen     web eval 'window.keys.join(",")'
step links       web links
step shot        web screenshot $OUT/page.png
step guilist     gui list
step follow      web click 'Second page'
step second      web eval 'location.pathname + " " + document.title'
step back        web back
step backed      web eval 'location.pathname'
step scroll      web scroll down 2
step badhost     web open http://127.0.0.1:9/ 30
step after       web info
step close       web close
sleep 2
step gone        web list
cp /tmp/.collabo-gui/logs/web.log $OUT/browser.log 2>/dev/null
echo done > $OUT/finished
