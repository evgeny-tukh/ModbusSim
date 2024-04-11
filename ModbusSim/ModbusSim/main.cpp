#define _CRT_SECURE_NO_WARNINGS

#include <Windows.h>
#include <WinSock.h>
#include <cstdint>
#include <stdio.h>
#include <vector>
#include <time.h>
#include <Shlwapi.h>

namespace {
	const uint16_t MODBUS_PORT = 502;
	const int YES = 1;
	const int BUF_SIZE = 300;

	enum ModbusFunction {
		READ_COIL_STATUS = 1,
		READ_INPUT_STATUS,
		READ_HOLDING_REGS,
		READ_INPUT_REGS,
		FORCE_SINGLE_COIL,
		PRESET_SINGLE_REG,
		FORCE_FEW_COILS = 15,
		PRESET_FEW_REGS,
	};

	struct IndexItem {
		uint16_t start;
		bool isFloat;

		IndexItem(uint16_t startParam, bool isFloatParam) : start(startParam), isFloat(isFloatParam) {}
	};

	struct Ctx {
		SOCKET listener { INVALID_SOCKET };
		SOCKET connection { INVALID_SOCKET };
		std::vector<uint16_t> holdings;
		std::vector<IndexItem> index;
		time_t lastResponse { 0 };
		time_t start{ 0 };
		time_t lastRecalc{ 0 };
		bool updateHoldings{ false };

		void setHolding(int ind, uint16_t value) {
			holdings[index[ind].start] = value;
		}

		void setHolding(int ind, float value) {
			union {
				float fVal;
				uint16_t nVal[2];
			};
			fVal = value;
			holdings[index[ind].start] = nVal[0];
			holdings[index[ind].start+1] = nVal[1];
		}
	};

	#pragma pack(1)
	struct MBAPHeader {
		uint16_t transactionID;
		uint16_t protoID;
		uint16_t len;
		uint8_t unitID;
	};

	struct ModbusRequest : MBAPHeader {
		uint8_t function;
	};

	struct HoldingReadResponse : ModbusRequest {
		uint8_t dataLen;
		uint16_t holdings[1];
	};

	#pragma pack()
}

void modifyHoldings(Ctx& ctx) {
	ctx.lastRecalc = time(nullptr);
	time_t offset = ctx.lastRecalc - ctx.start;
	float fVal = (offset % 10) * 10.0f;
	uint16_t nVal = (offset % 10) * 10;
	for (size_t i = 0; i < ctx.index.size(); ++i) {
		if (ctx.index[i].isFloat)
			ctx.setHolding((uint16_t) i, fVal);
		else
			ctx.setHolding((uint16_t) i, nVal);
	}
}

void readHoldingRegs(Ctx& ctx, char *request) {
	uint16_t *startAddrPtr = (uint16_t *)(request + sizeof(ModbusRequest));
	uint16_t *numOfRegsPtr = startAddrPtr + 1;
	uint16_t startAddr = htons(*startAddrPtr);
	uint16_t numOfRegs = htons(*numOfRegsPtr);

	// send response
	char responseBuf[512];
	HoldingReadResponse* response = (HoldingReadResponse*) responseBuf;
	memcpy(response, request, sizeof(ModbusRequest));				// copy request into response (up to function code inclusively)
	response->dataLen = (uint8_t) (numOfRegs * sizeof(uint16_t));
	for (uint16_t i = 0; i < numOfRegs; ++i) {
		response->holdings[i] = htons(ctx.holdings[startAddr + i]);
	}

	uint16_t length = sizeof(HoldingReadResponse) + (numOfRegs - 1) * sizeof(uint16_t);
	response->len = htons(length - 6);
	send(ctx.connection, responseBuf, length, 0);
}

void writeHoldingReg(Ctx& ctx, char* request) {
	uint16_t* startAddrPtr = (uint16_t*)(request + sizeof(ModbusRequest));
	uint16_t* regValuePtr = startAddrPtr + 1;
	uint16_t startAddr = htons(*startAddrPtr);
	uint16_t regValue = htons(*regValuePtr);

	ctx.holdings[startAddr] = regValue;
	// send response
	send(ctx.connection, request, sizeof(ModbusRequest) + 4, 0);
}

void writeHoldingRegs(Ctx& ctx, char* request) {
	uint16_t* startAddrPtr = (uint16_t*)(request + sizeof(ModbusRequest));
	uint16_t* numOfRegsPtr = startAddrPtr + 1;
	uint16_t startAddr = htons(*startAddrPtr);
	uint16_t numOfRegs = htons(*numOfRegsPtr);

	for (uint16_t i = 0; i < numOfRegs; ++i) {
		uint16_t *regValuePtr = (uint16_t*)(request + sizeof(ModbusRequest) + 5 + sizeof(uint16_t) * i);
		ctx.holdings[startAddr+i] = htons(*regValuePtr);
	}

	// send response
	char responseBuf[512];
	HoldingReadResponse* response = (HoldingReadResponse*)responseBuf;
	memcpy(response, request, sizeof(ModbusRequest) + 4);				// copy request into response (up to function code inclusively)
	response->len = htons(6);
	send(ctx.connection, responseBuf, sizeof(ModbusRequest) + 4, 0);
}

void readCfg(Ctx& ctx) {
	char path[MAX_PATH];
	GetModuleFileNameA(nullptr, path, sizeof(path));
	PathRenameExtensionA(path, ".ini");

	int numOfRegs = GetPrivateProfileInt("Settings", "numOfRegs", 0, path);
	ctx.updateHoldings = GetPrivateProfileInt("Settings", "updateHoldings", 0, path) != 0;

	for (int i = 0; i < numOfRegs; ++i) {
		char section[10];
		sprintf(section, "reg%05d", i + 1);
		int start = GetPrivateProfileInt(section, "start", 0, path);
		bool isFloat = GetPrivateProfileInt(section, "isFloat", 0, path) != 0;

		ctx.index.emplace_back(start, isFloat);
	}
}

int main(int argCount, char* args) {
	Ctx ctx;
	WSADATA data;

	ctx.start = time(nullptr);
	ctx.holdings.resize(1000);

	readCfg(ctx);

	for (size_t i = 0; i < ctx.holdings.size(); ++i)
		ctx.holdings[i] = (uint16_t)(i * 10 + 1);

	WSAStartup(0x0200, &data);

	ctx.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

	setsockopt(ctx.listener, SOL_SOCKET, SO_REUSEADDR, (const char*) &YES, sizeof(YES));
	setsockopt(ctx.listener, SOL_SOCKET, SO_KEEPALIVE, (const char*)&YES, sizeof(YES));
	setsockopt(ctx.listener, IPPROTO_TCP, TCP_NODELAY, (const char*)&YES, sizeof(YES));
	setsockopt(ctx.listener, SOL_SOCKET, SO_RCVBUF, (const char*)&BUF_SIZE, sizeof(BUF_SIZE));
	setsockopt(ctx.listener, SOL_SOCKET, SO_SNDBUF, (const char*)&BUF_SIZE, sizeof(BUF_SIZE));

	sockaddr_in local;
	local.sin_addr.S_un.S_addr = INADDR_ANY;
	local.sin_family = AF_INET;
	local.sin_port = htons(MODBUS_PORT);

	if (bind(ctx.listener, (const sockaddr*)&local, sizeof(local)) != 0) {
		printf("Unable to bind. Error %d\n", WSAGetLastError());
		return 100;
	}

	if (listen(ctx.listener, SOMAXCONN) != 0) {
		printf("Unable to switch to listening mode. Error %d\n", WSAGetLastError());
		return 101;
	}

	sockaddr_in peer;
	int size = sizeof(peer);

	while (true) {
		printf("Waiting for incoming connection...\n");
		ctx.connection = accept(ctx.listener, (sockaddr*)&peer, &size);

		if (ctx.connection == INVALID_SOCKET) {
			printf("Unable to accept incoming connection, error %d\n", WSAGetLastError());
			closesocket(ctx.listener);
			return 102;
		}

		printf("Incoming connection accepted, starting the conversation...\n");

		while (true) {
			char buffer[512];
			int received = recv(ctx.connection, buffer, sizeof(buffer), 0);

			if (received < 1) {
				if ((time(nullptr) - ctx.lastResponse) > 2) {
					printf("Connection timed out.\n");
					closesocket(ctx.connection);
					break;
				}
				continue;
			}

			ctx.lastResponse = time(nullptr);

			if (received > sizeof(ModbusRequest)) {
				ModbusRequest* request = (ModbusRequest*)buffer;

				auto now = time(nullptr);

				if (ctx.updateHoldings && (now > ctx.lastRecalc))
					modifyHoldings(ctx);

				switch (request->function) {
					case ModbusFunction::READ_HOLDING_REGS:
						readHoldingRegs(ctx, buffer); break;
					case ModbusFunction::PRESET_SINGLE_REG:
						writeHoldingReg(ctx, buffer); break;
					case ModbusFunction::PRESET_FEW_REGS:
						writeHoldingRegs(ctx, buffer); break;
				}
			}
		}
	}
}