#include "Camera.hpp"

#include <Geometry2d/Point.hpp>

REGISTER_CONFIGURABLE(Camera)

ConfigDouble* Camera::MHKF_radius_cutoff;
ConfigBool* Camera::use_MHKF;

void Camera::createConfiguration(Configuration* cfg) {
    MHKF_radius_cutoff = new ConfigDouble(cfg, "VisionFilter/Camera/MHKF_radius_cutoff", 1);
    use_MHKF = new ConfigBool(cfg, "VisionFilter/Camera/use_MHKF", true);
}

Camera::Camera() : isValid(false) {}

Camera::Camera(int cameraID)
    : isValid(true),
      cameraID(cameraID),
      kalmanRobotYellowList(maxRobotJerseyNum),
      kalmanRobotBlueList(maxRobotJerseyNum) {
}

bool Camera::getIsValid() {
    return isValid;
}

void Camera::processBallBounce(std::vector<WorldRobot> yellowRobots,
                               std::vector<WorldRobot> blueRobots) {
    for (KalmanBall& b : kalmanBallList) {
        Geometry2d::Point newVel;
        bool isCollision = BallBounce::CalcBallBounce(b, yellowRobots, blueRobots, newVel);

        if (isCollision) {
            b.setVel(newVel);
        }
    }
}

void Camera::updateWithFrame(RJ::Time calcTime,
                             std::vector<CameraBall> ballList,
                             std::vector<std::list<CameraRobot>> yellowRobotList,
                             std::vector<std::list<CameraRobot>> blueRobotList,
                             WorldBall& previousWorldBall,
                             std::vector<WorldRobot>& previousYellowWorldRobots,
                             std::vector<WorldRobot>& previousBlueWorldRobots) {
    // Prune list of balls and robots before doing anything
    removeInvalidBalls();
    removeInvalidRobots();

    updateBalls(calcTime, ballList, previousWorldBall);
    updateRobots(calcTime, yellowRobotList, blueRobotList,
                 previousYellowWorldRobots, previousBlueWorldRobots);
}

void Camera::updateWithoutFrame(RJ::Time calcTime) {
    removeInvalidBalls();
    removeInvalidRobots();

    for (KalmanBall& b : kalmanBallList) {
        b.predict(calcTime);
    }

    for (std::list<KalmanRobot>& robotList : kalmanRobotYellowList) {
        for (KalmanRobot& robot : robotList) {
            robot.predict(calcTime);
        }
    }

    for (std::list<KalmanRobot>& robotList : kalmanRobotBlueList) {
        for (KalmanRobot& robot : robotList) {
            robot.predict(calcTime);
        }
    }
}

void Camera::updateBalls(RJ::Time calcTime,
                         std::vector<CameraBall> ballList,
                         WorldBall& previousWorldBall) {
    // Make sure there are actually balls in the measurement
    // and only predict if that's the case
    if (ballList.size() == 0) {
        for (KalmanBall& b : kalmanBallList) {
            b.predict(calcTime);
        }

        return;
    }

    // We have some balls, so choose which updater to use
    if (*use_MHKF) {
        updateBallsMHKF(calcTime, ballList, previousWorldBall);
    } else {
        updateBallsAKF(calcTime, ballList, previousWorldBall);
    }
}

void Camera::updateBallsMHKF(RJ::Time calcTime,
                             std::vector<CameraBall> ballList,
                             WorldBall& previousWorldBall) {
    // If we have no existing filters, create a new one from average of everything
    // Easier than trying to figure out which ones are more than X meters away from each other
    // Only delays the filter collection by a camera frame or two
    if (kalmanBallList.size() == 0) {
        std::list<CameraBall> ballList2;
        std::copy(ballList.begin(), ballList.end(), std::back_inserter(ballList2));
        
        CameraBall avgBall = CameraBall::CombineBalls(ballList2);
        kalmanBallList.emplace_back(cameraID, calcTime, avgBall, previousWorldBall);

        return;
    }

    // TODO: Try merging some of the kalman filters together

    // Create list of bools corresponding to whether we have used this ball
    // as measurement yet
    std::vector<bool> usedCameraBall(ballList.size(), false);

    // Which camera balls to apply to which kalman ball
    std::vector<std::list<CameraBall>> appliedBallsList(kalmanBallList.size());

    int kalmanBallIdx = 0;
    for (KalmanBall& kalmanBall : kalmanBallList) {
        std::list<CameraBall>& measurementBalls = appliedBallsList.at(kalmanBallIdx);

        int cameraBallIdx = 0;
        for (CameraBall& cameraBall : ballList) {
            double dist = (kalmanBall.getPos() - cameraBall.getPos()).mag();

            if (dist < *MHKF_radius_cutoff) {
                measurementBalls.push_back(cameraBall);
                usedCameraBall.at(cameraBallIdx) = true;
            }
            cameraBallIdx++;
        }

        kalmanBallIdx++;
    }

    kalmanBallIdx = 0;
    for (KalmanBall& kalmanBall : kalmanBallList) {
        std::list<CameraBall>& measurementBalls = appliedBallsList.at(kalmanBallIdx);

        // We had at least one measurement near this ball
        if (measurementBalls.size() > 0) {
            CameraBall avgBall = CameraBall::CombineBalls(measurementBalls);
            kalmanBall.predictAndUpdate(calcTime, avgBall);

        // There aren't any measurements so just predict
        } else {
            kalmanBall.predict(calcTime);
        }
    }

    for (int i = 0; i < ballList.size(); i++) {
        CameraBall& cameraBall = ballList.at(i);
        bool wasUsed = usedCameraBall.at(i);

        if (!wasUsed) {
            kalmanBallList.emplace_back(cameraID, calcTime, cameraBall, previousWorldBall);
        }
    }
}

void Camera::updateBallsAKF(RJ::Time calcTime,
                            std::vector<CameraBall> ballList,
                            WorldBall& previousWorldBall) {
    std::list<CameraBall> ballList2;
    std::copy(ballList.begin(), ballList.end(), std::back_inserter(ballList2));

    // If we have no existing filters, create a new one from average of everything
    if (kalmanBallList.size() == 0) {
        CameraBall avgBall = CameraBall::CombineBalls(ballList2);
        kalmanBallList.emplace_back(cameraID, calcTime, avgBall, previousWorldBall);

        return;
    }

    // Average everything and add as measuremnet
    CameraBall avgBall = CameraBall::CombineBalls(ballList2);
    // Kinda cheating, but we are only keeping a single element in the list
    kalmanBallList.front().predictAndUpdate(calcTime, avgBall);
}

void Camera::updateRobots(RJ::Time calcTime,
                          std::vector<std::list<CameraRobot>> yellowRobotList,
                          std::vector<std::list<CameraRobot>> blueRobotList,
                          std::vector<WorldRobot>& previousYellowWorldRobots,
                          std::vector<WorldRobot>& previousBlueWorldRobots) {

    for (int i = 0; i < maxRobotJerseyNum; i++) {
        std::list<CameraRobot> singleYellowRobotList = yellowRobotList.at(i);
        std::list<CameraRobot> singleBlueRobotList = blueRobotList.at(i);

        // Make sure we actually have robots for the yellow team
        if (singleYellowRobotList.size() == 0) {
            for (KalmanRobot& robot : kalmanRobotYellowList.at(i)) {
                robot.predict(calcTime);
            }

        // If we do, do the fancy updates
        } else {
            if (*use_MHKF) {
                updateRobotsMHKF(calcTime,
                                 singleYellowRobotList,
                                 previousYellowWorldRobots.at(i),
                                 kalmanRobotYellowList.at(i));
            } else {
                updateRobotsAKF(calcTime,
                                singleYellowRobotList,
                                previousYellowWorldRobots.at(i),
                                kalmanRobotYellowList.at(i));
            }
        }

        // Make sure we actually have robots for the blue team
        if (singleBlueRobotList.size() == 0) {
            for (KalmanRobot& robot : kalmanRobotBlueList.at(i)) {
                robot.predict(calcTime);
            }

        // If we do, do the fancy updates
        } else {
            if (*use_MHKF) {
                updateRobotsMHKF(calcTime,
                                 singleBlueRobotList,
                                 previousBlueWorldRobots.at(i),
                                 kalmanRobotBlueList.at(i));
            } else {
                updateRobotsAKF(calcTime,
                                singleBlueRobotList,
                                previousBlueWorldRobots.at(i),
                                kalmanRobotBlueList.at(i));
            }
        }
    }
}

void Camera::updateRobotsMHKF(RJ::Time calcTime,
                              std::list<CameraRobot> singleRobotList,
                              WorldRobot& previousWorldRobot,
                              std::list<KalmanRobot>& singleKalmanRobotList) {
    // If we have no existing filters, create a new one from average of everything
    // Easier than trying to figure out which ones are more than X meters away from each other
    // Only delays the filter collection by a camera frame or two
    if (singleKalmanRobotList.size() == 0) {
        std::list<CameraRobot> singleRobotList2;
        std::copy(singleRobotList.begin(), singleRobotList.end(), std::back_inserter(singleRobotList2));

        CameraRobot avgRobot = CameraRobot::CombineRobots(singleRobotList2);
        singleKalmanRobotList.emplace_back(cameraID, calcTime, avgRobot, previousWorldRobot);

        return;
    }

    // TODO: Try merging some of the kalman filters together

    // Create list of bools corresponding to whether we have used this Robot
    // as measurement yet
    std::vector<bool> usedCameraRobot(singleRobotList.size(), false);

    // Which camera robots to apply to which kalman Robot
    std::vector<std::list<CameraRobot>> appliedRobotsList(singleKalmanRobotList.size());

    // Apply camera robots to different kalman filters based off a fixed distance
    // A single camera robots can go to multiple different kalman robots
    int kalmanRobotIdx = 0;
    for (KalmanRobot& kalmanRobot : singleKalmanRobotList) {
        std::list<CameraRobot>& measurementRobot = appliedRobotsList.at(kalmanRobotIdx);

        int cameraRobotIdx = 0;
        for (CameraRobot& cameraRobot : singleRobotList) {
            double dist = (kalmanRobot.getPos() - cameraRobot.getPos()).mag();

            if (dist < *MHKF_radius_cutoff) {
                measurementRobot.push_back(cameraRobot);
                usedCameraRobot.at(cameraRobotIdx) = true;
            }
            cameraRobotIdx++;
        }

        kalmanRobotIdx++;
    }

    kalmanRobotIdx = 0;
    for (KalmanRobot& kalmanRobot : singleKalmanRobotList) {
        std::list<CameraRobot>& measurementRobots = appliedRobotsList.at(kalmanRobotIdx);

        // We had at least one measurement near this Robot
        if (measurementRobots.size() > 0) {
            CameraRobot avgRobot = CameraRobot::CombineRobots(measurementRobots);
            kalmanRobot.predictAndUpdate(calcTime, avgRobot);

        // There aren't any measurements so just predict
        } else {
            kalmanRobot.predict(calcTime);
        }
    }

    int cameraRobotIdx = 0;
    for (CameraRobot& cameraRobot : singleRobotList) {
        bool wasUsed = usedCameraRobot.at(cameraRobotIdx);

        if (!wasUsed) {
            singleKalmanRobotList.emplace_back(cameraID, calcTime, cameraRobot, previousWorldRobot);
        }
        
        cameraRobotIdx++;
    }
}

void Camera::updateRobotsAKF(RJ::Time calcTime,
                             std::list<CameraRobot> singleRobotList,
                             WorldRobot& previousWorldRobot,
                             std::list<KalmanRobot>& singleKalmanRobotList) {
    
    std::list<CameraRobot> singleRobotList2;
    std::copy(singleRobotList.begin(), singleRobotList.end(), std::back_inserter(singleRobotList2));

    // If we have no existing filters, create a new one from average of everything
    if (singleKalmanRobotList.size() == 0) {
        CameraRobot avgRobot = CameraRobot::CombineRobots(singleRobotList2);
        singleKalmanRobotList.emplace_back(cameraID, calcTime, avgRobot, previousWorldRobot);

        return;
    }

    // Average everything and add as measuremnet
    CameraRobot avgRobot = CameraRobot::CombineRobots(singleRobotList2);
    // Kinda cheating, but we are only keeping a single element in the list
    singleKalmanRobotList.front().predictAndUpdate(calcTime, avgRobot);
}

void Camera::removeInvalidBalls() {
    // Remove all balls that are unhealthy
    kalmanBallList.remove_if([](KalmanBall& b) -> bool { return b.isUnhealthy(); } );
}

void Camera::removeInvalidRobots() {
    // Remove all the robots that are unhealthy
    for (std::list<KalmanRobot> robotList : kalmanRobotBlueList) {
        robotList.remove_if([](KalmanRobot& r) -> bool {return r.isUnhealthy(); } );
    }

    for (std::list<KalmanRobot> robotList : kalmanRobotYellowList) {
        robotList.remove_if([](KalmanRobot& r) -> bool {return r.isUnhealthy(); } );
    }
}

void Camera::predictAllRobots(RJ::Time calcTime, std::vector<std::list<KalmanRobot>>& robotListList) {
    for (std::list<KalmanRobot>& robotList : robotListList) {
        for (KalmanRobot& robot : robotList) {
            robot.predict(calcTime);
        }
    }
}

std::list<KalmanBall> Camera::getKalmanBalls() {
    return kalmanBallList;
}

std::vector<std::list<KalmanRobot>> Camera::getKalmanRobotsYellow() {
    return kalmanRobotYellowList;
}

std::vector<std::list<KalmanRobot>> Camera::getKalmanRobotsBlue() {
    return kalmanRobotBlueList;
}