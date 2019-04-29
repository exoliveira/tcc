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

float getCx(float x) {
	if (global_cx == 0) {
		global_cx = x + 30;
	}
	return global_cx;
}

float getCy(float y) {
	if (global_cy == 0) {
		global_cy = y;
	}
	return global_cy;
}
int main()
{
	std::cout << "Hello World!\n";
	//auto connection = MavLinkConnection::connectRemoteUdp("qgc", "127.0.0.1", "127.0.0.1", 14560);
	auto connection = MavLinkConnection::connectRemoteUdp("PX4_2", "10.0.75.1", "172.18.0.2", 14557);
	auto connection2 = MavLinkConnection::connectRemoteUdp("PX4_1", "127.0.0.1", "127.0.0.1", 14557);
	
	auto vehicle = std::make_shared<MavLinkVehicle>(166, 1);
	vehicle->connect(connection);
	vehicle->startHeartbeat();

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
		return 0;
	}
	if (!vehicle->takeoff(targetAlt).wait(3000, &rc) || !rc) {
		printf("takeoff command failed\n");
		return 0;
	}
	int version = vehicle->getVehicleStateVersion();
	while (true) {
		int newVersion = vehicle->getVehicleStateVersion();
		//printf("My new version is %d\n",newVersion);
		//printf("But the old one is %d\n", version);
		if (version != newVersion) {
			VehicleState state = vehicle->getVehicleState();
			float alt = state.local_est.pos.z;
			if (alt >= targetAlt - delta && alt <= targetAlt + delta)
			{
				reached = true;
				printf("Target altitude reached\n");
				break;
			}
			//std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			std::cout << "Current altitude is " << alt << " and final is " << targetAlt << std::endl;
		}
		else {
			std::cout << "Sleeping ........ (by exo)" << std::endl;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}

	double orbitSpeed = 4;
	int counter = 0;

	vehicle->setMessageInterval((int)MavLinkMessageIds::MAVLINK_MSG_ID_LOCAL_POSITION_NED, 30);
	vehicle->requestControl();
	std::this_thread::sleep_for(std::chrono::seconds(5));
	std::cout << "Control requested then orbital move is starting  ..." << std::endl;
	int subscription = vehicle->getConnection()->subscribe(
		[&](std::shared_ptr<MavLinkConnection> con, const MavLinkMessage& m) {
		if (m.msgid == (int)MavLinkMessageIds::MAVLINK_MSG_ID_LOCAL_POSITION_NED) {
			counter++;
			if (counter > 500) {
				vehicle->getConnection()->unsubscribe(subscription);
			}
			std::cout << "Counter is now " << counter << std::endl;
			MavLinkLocalPositionNed localPos;
			localPos.decode(m);

			float x = localPos.x;
			float y = localPos.y;
			std::cout << "Current x and y " << x << " and " << y << std::endl;
			auto cx = getCx(x);
			auto cy = getCy(y);
			std::cout << "Center located @ x = " << cx << " and y = " << cy << std::endl;
			float dx = x - cx;
			float dy = y - cy;
			float angle = atan2(dy, dx);
			if (angle < 0) angle += M_PIf * 2;
			float tangent = angle + M_PI_2;
			double newvx = orbitSpeed * cos(tangent);
			double newvy = orbitSpeed * sin(tangent);
			float heading = angle + M_PIf;
			std::cout << "New vx = " << newvx << std::endl;
			vehicle->moveByLocalVelocityWithAltHold(newvx, newvy, targetAlt, true, heading);
		}
	});



	vehicle->returnToHome();
	while (true) {
		VehicleState state = vehicle->getVehicleState();
		float alt = state.local_est.pos.z;
		//std::cout << "Current altitude is " << alt << std::endl;
	}

	vehicle->releaseControl();
}

// Executar programa: Ctrl + F5 ou Menu Depurar > Iniciar Sem Depuração
// Depurar programa: F5 ou menu Depurar > Iniciar Depuração

// Dicas para Começar: 
//   1. Use a janela do Gerenciador de Soluções para adicionar/gerenciar arquivos
//   2. Use a janela do Team Explorer para conectar-se ao controle do código-fonte
//   3. Use a janela de Saída para ver mensagens de saída do build e outras mensagens
//   4. Use a janela Lista de Erros para exibir erros
//   5. Ir Para o Projeto > Adicionar Novo Item para criar novos arquivos de código, ou Projeto > Adicionar Item Existente para adicionar arquivos de código existentes ao projeto
//   6. No futuro, para abrir este projeto novamente, vá para Arquivo > Abrir > Projeto e selecione o arquivo. sln
