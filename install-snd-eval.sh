#!/bin/bash
sudo apt-get -y install libasound2-dev
sudo apt-get -y install libfftw3-dev
sudo cp ./sound-eval/sound-eval.sh /usr/local/bin/
sudo cp ./sound-eval/soundEval /usr/local/bin/
sudo cp ./sound-eval/sound-eval.service /etc/systemd/system/ 
sudo chmod 744 /usr/local/bin/sound-eval.sh
sudo chown root.root /usr/local/bin/sound-eval.sh
sudo systemctl enable sound-eval.service

