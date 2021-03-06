g++ -I/opt/adapteva/esdk/tools/host/include -I/usr/local/include -O3 -g0 -Wall -c -fmessage-length=0 -fopenmp -MMD -MP EpFaceHost/cpp/ep_cascade_detector.cpp -o release/cpp/ep_cascade_detector.o
gcc -I/opt/adapteva/esdk/tools/host/include -I/usr/local/include -O3 -g0 -Wall -c -fmessage-length=0 -fopenmp -MMD -MP -std=c99 EpFaceHost/c/ep_cascade_detector.c -o release/c/ep_cascade_detector.o
gcc -I/opt/adapteva/esdk/tools/host/include -I/usr/local/include -O3 -g0 -Wall -c -fmessage-length=0 -fopenmp -MMD -MP -std=c99 EpFaceHost/c/ep_emulator.c -o release/c/ep_emulator.o
g++ -I/opt/adapteva/esdk/tools/host/include -I/usr/local/include -O3 -g0 -Wall -c -fmessage-length=0 -fopenmp -MMD -MP EpFaceHost/main.cpp -o release/main.o
g++ -L/opt/adapteva/esdk/tools/host/lib -z origin -fopenmp release/cpp/ep_cascade_detector.o release/c/ep_cascade_detector.o release/c/ep_emulator.o release/main.o -o release/EpFaceHost -lopencv_core -lopencv_highgui -lopencv_imgproc -lopencv_objdetect -lpthread -lm -le-hal -lrt -le-loader

e-gcc EpFaceCore_commonlib/src/device_cascade_detector.c -O3 -ffast-math -Wall -std=c99 -T/opt/adapteva/esdk/bsps/current/internal.ldf -le-lib -o release/epiphany.elf

