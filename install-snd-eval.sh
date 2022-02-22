#!/bin/bash
sudo apt-get -y install libasound2-dev
sudo apt-get -y install libfftw3-dev
sudo systemctl stop sound-eval.service
sudo systemctl disable sound-eval.service
sudo cp ./sound-eval/sound-eval.sh /usr/local/bin/
uname -m | grep -q 'aarch64' && SNDEVBIN="soundEval64" || SNDEVBIN="soundEval"
sudo cp ./sound-eval/$SNDEVBIN /usr/local/bin/soundEval
sudo cp ./sound-eval/sound-eval.service /etc/systemd/system/
sudo chmod 744 /usr/local/bin/sound-eval.sh
sudo chown root.root /usr/local/bin/sound-eval.sh
sudo systemctl enable sound-eval.service
sudo systemctl start sound-eval.service
