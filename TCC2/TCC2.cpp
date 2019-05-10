// TCC2.cpp : Este arquivo contém a função 'main'. A execução do programa começa e termina ali.
//Source code here!!!! https://github.com/Microsoft/AirSim/tree/master/MavLinkCom
// Local PX4 log: C:\Users\Euler\AppData\Local\Packages\CanonicalGroupLimited.Ubuntu16.04onWindows_79rhkp1fndgsc\LocalState\rootfs\home\exo\PX4\Firmware\build\posix_sitl_ekf2\tmp\rootfs\fs\microsd\log\2019-04-14
// PX4 log review: https://review.px4.io/plot_app?log=12d943c9-95e8-47b0-b62b-57a60d242787

#include "pch.h"
#include "Utils.hpp"
#include "FileSystem.hpp"
#include "MavLinkConnection.hpp"
#include "MavLinkVehicle.hpp"
#include "MavLinkMessages.hpp"
#include "MavLinkLog.hpp"
#include <iostream>
#include <vector>
#include <string.h>
#include <functional>
#include <mutex>
#include <map>
#include <ctime>
#include <thread>
STRICT_MODE_OFF
#include "json.hpp"
STRICT_MODE_ON

#if defined(_WIN32) || ((defined __cplusplus) && (__cplusplus >= 201700L))
#include <filesystem>
#define USE_CPP_FILESYSTEM
#else
#undef USE_CPP_FILESYSTEM
#endif

/* enable math defines on Windows */

#ifndef M_PI_2
#define M_PI_2     1.57079632679489661923   // pi/2
#endif

using namespace mavlinkcom;

// this switch controls whether we turn off the RC remote active link loss detection 
// if you do not have radio connected this is needed to stop "failsafe" override in pixhawk 
// from kicking in when you try and fly.
bool noRadio = false;
bool unitTest = false;
bool verbose = false;
bool nsh = false;
bool noparams = false;
std::string logDirectory;
std::string ifaceName;
bool jsonLogFormat = false;
bool csvLogFormat = false;
bool convertExisting = false;
std::vector<int> filterTypes;
std::shared_ptr<MavLinkFileLog> inLogFile;
std::shared_ptr<MavLinkFileLog> outLogFile;
std::thread telemetry_thread;
bool telemetry = false;
std::mutex logLock;
std::stringstream initScript;

std::shared_ptr<MavLinkConnection> droneConnection;
std::shared_ptr<MavLinkConnection> logConnection;
std::shared_ptr<MavLinkVehicle> mavLinkVehicle;

#if defined(USE_CPP_FILESYSTEM)

//can't use experimental stuff on Linux because of potential ABI issues
#if defined(_WIN32) || ((defined __cplusplus) && (__cplusplus < 201700L))
using namespace std::experimental::filesystem;
#else
using namespace std::filesystem;
#endif

#endif

float global_cx = 0.0;
float global_cy = 0.0;

float global_prey_x;
float global_prey_y;
float global_prey_z;

float getCx(float x) {
	if (global_cx == 0) {
		global_cx = x -5;
	}
	return global_cx;
}

float getCy(float y) {
	if (global_cy == 0) {
		global_cy = y;
	}
	return global_cy;
}

void prey(std::shared_ptr<MavLinkVehicle> vehicle) {
	std::cout << "Starting prey ..." << std::endl;
	std::cout << "Prey: connection created!! " << std::endl;

	std::this_thread::sleep_for(std::chrono::seconds(5));

	VehicleState state = vehicle->getVehicleState();
	printf("Home position is %s, %f,%f,%f\n", state.home.is_set ? "set" : "not set",
		state.home.global_pos.lat, state.home.global_pos.lon, state.home.global_pos.alt);

	bool rc = false;
	float targetAlt = -50.0; // Target altitude is negative (See NED -North East Down - coordinates)
	bool reached = false;
	float delta = 1;
	if (!vehicle->armDisarm(true).wait(3000, &rc) || !rc) {
		printf("arm command failed\n");
	}
	if (!vehicle->takeoff(targetAlt).wait(3000, &rc) || !rc) {
		printf("takeoff command failed\n");
	}
	int version = vehicle->getVehicleStateVersion();
	while (true) {
		int newVersion = vehicle->getVehicleStateVersion();
		if (version != newVersion) {
			VehicleState state = vehicle->getVehicleState();
			float alt = state.local_est.pos.z;
			if (alt >= targetAlt - delta && alt <= targetAlt + delta)
			{
				reached = true;
				printf("Target altitude reached\n");
				break;
			}
		}
		else {
			std::cout << "Sleeping ........ (by exo)" << std::endl;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}

	double orbitSpeed = 1;
	int counter = 0;

	vehicle->setMessageInterval((int)MavLinkMessageIds::MAVLINK_MSG_ID_LOCAL_POSITION_NED, 30);
	vehicle->requestControl();
	std::this_thread::sleep_for(std::chrono::seconds(60));

	auto promise = std::promise<std::string>();
	auto future = promise.get_future();

	int subscription = vehicle->getConnection()->subscribe(
		[&](std::shared_ptr<MavLinkConnection> con, const MavLinkMessage& m) {
		if (m.msgid == (int)MavLinkMessageIds::MAVLINK_MSG_ID_LOCAL_POSITION_NED) {
			counter++;
			if (counter > 3000) {
				vehicle->getConnection()->unsubscribe(subscription);
				vehicle->releaseControl();
				promise.set_value("Done!");
			}
			else {
				MavLinkLocalPositionNed localPos;
				localPos.decode(m);

				global_prey_x = localPos.x;
				global_prey_y = localPos.y;
				global_prey_z = localPos.z;
				auto cx = getCx(global_prey_x);
				auto cy = getCy(global_prey_y);
				float dx = global_prey_x - cx;
				float dy = global_prey_y - cy;
				float angle = atan2(dy, dx);
				if (angle < 0) angle += M_PIf * 2;
				float tangent = angle + M_PI_2;
				double newvx = orbitSpeed * cos(tangent);
				double newvy = orbitSpeed * sin(tangent);
				float heading = angle + M_PIf;

				vehicle->getVehicleStateVersion();
				VehicleState state = vehicle->getVehicleState();
				vehicle->moveByLocalVelocityWithAltHold(newvx, newvy, targetAlt, true, heading);
			}
		}
	});
	std::cout << "Message from future: " << future.get() << std::endl;

	vehicle->returnToHome();
}

void predator(std::shared_ptr<MavLinkVehicle> vehicle) {
	std::cout << "Starting predator ..." << std::endl;
	std::this_thread::sleep_for(std::chrono::seconds(30));
	std::cout << '\a';
	std::cout << "Predator going to altitude " << global_prey_z << std::endl;
	VehicleState state = vehicle->getVehicleState();
	printf("Home position is %s, %f,%f,%f\n", state.home.is_set ? "set" : "not set",
		state.home.global_pos.lat, state.home.global_pos.lon, state.home.global_pos.alt);

	bool rc = false;
	bool reached = false;
	float delta = 1;
	if (!vehicle->armDisarm(true).wait(3000, &rc) || !rc) {
		printf("arm command failed\n");
	}
	if (!vehicle->takeoff(global_prey_z).wait(3000, &rc) || !rc) {
		printf("takeoff command failed\n");
	}
	int version = vehicle->getVehicleStateVersion();
	while (true) {
		int newVersion = vehicle->getVehicleStateVersion();
		if (version != newVersion) {
			VehicleState state = vehicle->getVehicleState();
			float alt = state.local_est.pos.z;
			if (alt >= global_prey_z - delta && alt <= global_prey_z + delta)
			{
				reached = true;
				printf("Target altitude reached\n");
				break;
			}
		}
		else {
			std::cout << "Sleeping ........ (by exo)" << std::endl;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}

	double orbitSpeed = 4;
	int counter = 0;

	vehicle->setMessageInterval((int)MavLinkMessageIds::MAVLINK_MSG_ID_LOCAL_POSITION_NED, 10);
	vehicle->requestControl();
	std::this_thread::sleep_for(std::chrono::seconds(10));

	auto promise = std::promise<std::string>();
	auto future = promise.get_future();

	int subscription = vehicle->getConnection()->subscribe(
		[&](std::shared_ptr<MavLinkConnection> con, const MavLinkMessage& m) {
		if (m.msgid == (int)MavLinkMessageIds::MAVLINK_MSG_ID_LOCAL_POSITION_NED) {
			counter++;
			if (counter > 3000) {
				vehicle->getConnection()->unsubscribe(subscription);
				vehicle->releaseControl();
				promise.set_value("Done!");
			}
			else {
				MavLinkLocalPositionNed localPos;
				localPos.decode(m);

				//global_prey_x = localPos.x;
				//global_prey_y = localPos.y;

				// Why did I have to add 10 ?
				// because @ settings.json prey position is (x=10,y=0,z=0) for 
				// and predator position is (x=0,y=0,z=0)
				float dx = global_prey_x - localPos.x  - 10;
				float dy = global_prey_y - localPos.y;
				float angle = atan2(dy, dx);
				if (angle < 0) angle += M_PIf * 2;
				float tangent = angle + M_PI_2;
				double newvx = orbitSpeed * cos(tangent);
				double newvy = orbitSpeed * sin(tangent);
				float heading = angle + M_PIf;
				std::cout << "x = " << localPos.x << " y = " << localPos.y << " dx = " << dx << " dy = " << dy << " angle = " << angle << std::endl;

				vehicle->getVehicleStateVersion();
				VehicleState state = vehicle->getVehicleState();
				//vehicle->moveByLocalVelocityWithAltHold(newvx, newvy, targetAlt, true, heading);
				vehicle->moveByLocalVelocityWithAltHold(0, 0, global_prey_z, true, heading);
			}
		}
	});
	std::cout << "Message from future: " << future.get() << std::endl;

	vehicle->returnToHome();

}

int main()
{
	auto connectionPredator = MavLinkConnection::connectRemoteUdp("PX4_1", "127.0.0.1", "127.0.0.1", 14557);
	auto vehiclePredator = std::make_shared<MavLinkVehicle>(166, 1);
	vehiclePredator->connect(connectionPredator);
	vehiclePredator->startHeartbeat();
	std::thread drone2(predator, vehiclePredator);

	auto connectionPrey = MavLinkConnection::connectRemoteUdp("PX4_2", "10.0.75.1", "172.18.0.2", 14557);
	auto vehiclePrey = std::make_shared<MavLinkVehicle>(167, 1);
	vehiclePrey->connect(connectionPrey);
	vehiclePrey->startHeartbeat();
	std::thread drone1(prey, vehiclePrey);

	std::cout << "Main thread joining ..." << std::endl;
	drone2.join();
	drone1.join();
	std::cout << "Main thread ended!" << std::endl;
	return 0;
}