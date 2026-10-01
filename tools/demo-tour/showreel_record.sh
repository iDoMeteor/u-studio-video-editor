#!/bin/bash
# Raw footage for the showreel: sequential recording runs (no narration, no saving).
S=${1:?usage: showreel_record.sh <runs dir>}; B=/home/jj/Repos/u-studio-video-editor.worktrees/demos; D=$B/tools/demo-tour
BIN=$B/builddir/src/app/u-studio-video-editor; MEDIA=$HOME/.cache/ustudio-demo-media
run() { # name script skip dropins_off stop
  echo "$(date +%T) start $1" >> $S/record.log
  (cd $D && TOUR_SCRIPT=$D/$2 TOUR_SKIP=$3 TOUR_DROPINS_OFF=$4 TOUR_STOP_AFTER="$5" TOUR_MEDIA=$MEDIA TOUR_BIN=$BIN ./run_tour.sh $S/$1 1)
  echo "$(date +%T) end $1 tb=$(grep -c Traceback $S/$1/run.log) assert=$(grep -c Assertion $S/$1/app.stderr)" >> $S/record.log
}
run r1-edit-pip tour.py sequence,titles effects,titles "Edit Transform"
run r2-keyframes keyframes.py "" titles ""
run r3-trans1 transitions.py "" titles ""
run r4-trans2 transitions2.py "" titles ""
run r5-trans3 transitions3.py "" titles ""
run r6-fx1 effects1.py "" titles ""
run r7-fx2 effects2.py "" titles ""
run r8-anim animated.py "" effects ""
run r9-titles tour.py transform,sequence effects "Save the title"
run r10-captions captions.py "" effects ""
run r11-gpu gpu.py "" effects,titles "The difference"
run r12-media tour.py transform,titles effects,titles "Relink"
echo "$(date +%T) ALL DONE" >> $S/record.log
