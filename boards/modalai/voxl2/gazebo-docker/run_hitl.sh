#!/bin/bash

VOLUMES="-v /dev:/dev -v /tmp/.X11-unix:/tmp/.X11-unix:ro"

docker run -it --entrypoint "/bin/bash" --rm --privileged --net=host --ipc=host -e DISPLAY=$DISPLAY ${VOLUMES} ubuntu_docker -c "cd /root/PX4-Autopilot && source Tools/simulation/gazebo-classic/setup_gazebo.bash $(pwd) $(pwd)/build/px4_sitl_default && gazebo Tools/simulation/gazebo-classic/sitl_gazebo-classic/worlds/hitl_iris.world"
