

#include <KR3/main.h>
#include <KR3/js/js.h>
#include <KR3/fs/file.h>
#include <KR3/util/path.h>
#include <KR3/util/parameter.h>
#include <KR3/parser/jsonparser.h>
#include <KR3/data/map.h>
#include <KR3/data/set.h>
#include <KR3/io/selfbufferedstream.h>
#include <KR3/net/ipaddr.h>
#include <KR3/wl/windows.h>
#include <KR3/msg/pump.h>
#include <KRWin/handle.h>
#include <KRWin/hook.h>

// #include "ChakraDebugService.h"

 #define USE_EDGEMODE_JSRT
 #include <jsrt.h>

#include <WinSock2.h>

#include "jsctx.h"
#include "nativepointer.h"
#include "reverse.h"
#include "console.h"
#include "fs.h"
#include "nethook.h"
#include "mcf.h"
#include "require.h"
#include "native.h"

#pragma comment(lib, "chakrart.lib")

using namespace kr;

class SingleInstanceLimiter
{
private:
	HANDLE m_mutex;

public:
	SingleInstanceLimiter() noexcept
	{
	}
	~SingleInstanceLimiter() noexcept
	{
		ReleaseSemaphore(m_mutex, 1, nullptr);
	}
	void create(pcstr16 name) noexcept
	{
		m_mutex = CreateSemaphoreW(nullptr, 1, 1, wide(name));
		int err = GetLastError();
		if (ERROR_ALREADY_EXISTS == err)
		{
			cout << "BDSX: (id=" << (Utf16ToAnsi)(Text16)name << ") is Already executing" << endl;
			cout << "BDSX: Wait the process terminating..." << endl;
			WaitForSingleObject(m_mutex, INFINITE);
			cout << "BDSX: Previous process terminated" << endl;
		}
		else
		{
			WaitForSingleObject(m_mutex, INFINITE);
		}
	}
};

namespace
{
	//JsDebugService s_debug;
	//JsDebugProtocolHandler s_debugHandler;
	win::Module* s_module = win::Module::getModule(nullptr);
	hook::IATHookerList s_iatChakra(s_module, "chakra.dll");
	hook::IATHookerList s_iatWS2_32(s_module, "WS2_32.dll");
	hook::IATHookerList s_iatUcrtbase(s_module, "api-ms-win-crt-heap-l1-1-0.dll");
	Map<Text, AText> s_uuidToPackPath;
	SingleInstanceLimiter s_singleInstance;
}

void catchException() noexcept
{
	JsValueRef exception;
	if (JsGetAndClearException(&exception) == JsNoError)
	{
		JsScope scope;
		g_native->fireError((JsRawData)exception);
		JsRelease(exception, nullptr);
	}
}

int WSAAPI closesocketHook(SOCKET s) noexcept
{
	removeBindList(s);
	return closesocket(s);
}

// It cannot use, minecraft trys to open with several ports
int CALLBACK bindHook(
	SOCKET s,
	const sockaddr* name,
	int namelen
)
{
	int res = bind(s, name, namelen);
	if (res == 0) addBindList(s);
	return res;
}
int CALLBACK recvfromHook(
	SOCKET s, char* buf, int len, int flags,
	sockaddr* from, int* fromlen
)
{
	int res = recvfrom(s, buf, len, flags, from, fromlen);
	if (res == SOCKET_ERROR) return SOCKET_ERROR;
	if (!isContextExisted()) return res;

	Ipv4Address& ip = (Ipv4Address&)((sockaddr_in*)from)->sin_addr;

	addTraffic(ip, res);
	if (g_native->isFilted(ip))
	{
		*fromlen = 0;
		WSASetLastError(WSAECONNREFUSED);
		return -1;
	}
	return res;
}
JsErrorCode CALLBACK JsCreateRuntimeHook(
	JsRuntimeAttributes attributes,
	JsThreadServiceCallback threadService,
	JsRuntimeHandle* runtime) noexcept
{
	JsErrorCode err = JsCreateRuntime(attributes, threadService, runtime);
	if (err == JsNoError)
	{
		JsRuntime::setRuntime(*runtime);
		static EventPump* pump;
		pump = EventPump::getInstance();

		g_mcf.hookOnUpdate([] {
			JsScope scope;
			try
			{
				pump->processOnce();
			}
			catch (QuitException&)
			{
				g_mcf.stopServer();
			}
			});

		JsonParser parser(File::open(u"valid_known_packs.json"));
		parser.array([&](size_t idx){
			AText uuid;
			AText path;
			parser.fields([&](JsonField&field){
				field("uuid", &uuid);
				field("path", &path);
			});
			if (path == nullptr) return;
			if (uuid == nullptr) return;

			s_uuidToPackPath[uuid] = move(path);
		});
		
		//JsDebugServiceCreate(&s_debug);
		//JsDebugProtocolHandlerCreate(*runtime, &s_debugHandler);
		//JsDebugServiceRegisterHandler(s_debug, "minecraft", s_debugHandler, false);
		//JsDebugServiceListen(s_debug, 9229);
		//JsDebugProtocolHandlerWaitForDebugger(s_debugHandler);
	}
	return err;
}
JsErrorCode CALLBACK JsDisposeRuntimeHook(JsRuntimeHandle runtime) noexcept
{
	destroyJsContext();
	return JsDisposeRuntime(runtime);
}
JsErrorCode CALLBACK JsCreateContextHook(JsRuntimeHandle runtime, JsContextRef* newContext) noexcept
{
	JsErrorCode err = JsCreateContext(runtime, newContext);
	if (err == JsNoError)
	{
		createJsContext(*newContext);
	}
	return err;
}
JsErrorCode CALLBACK JsRunScriptHook(
	const wchar_t* script,
	JsSourceContext sourceContext,
	const wchar_t* sourceUrl,
	JsValueRef* result) noexcept
{
	constexpr size_t UUID_LEN = 36;
	auto path = (Text16)unwide(sourceUrl);
	if (path.size() >= UUID_LEN)
	{
		TText uuid = TText::concat(toUtf8(path.cut(UUID_LEN)));
		auto iter = s_uuidToPackPath.find(uuid);
		if (iter != s_uuidToPackPath.end())
		{
			path.subarr_self(UUID_LEN);
			Text16 rpath = path.subarr(path.find_e(u'/'));
			pcstr16 remove_end = rpath.find_r(u'_');
			if (remove_end != nullptr) rpath.cut_self(remove_end);

			TText16 newpath = TText16::concat(utf8ToUtf16(iter->second), rpath, nullterm);
			JsErrorCode err = JsRunScript(script, sourceContext, wide(newpath.data()), result);
			if (err != JsNoError) catchException();
			return err;
		}
	}
	JsErrorCode err = JsRunScript(script, sourceContext, sourceUrl, result);
	if (err != JsNoError) catchException();
	return err;
}
JsErrorCode CALLBACK JsCallFunctionHook(
	JsValueRef function,
	JsValueRef* arguments,
	unsigned short argumentCount,
	JsValueRef* result) noexcept
{
	JsErrorCode err = JsCallFunction(function, arguments, argumentCount, result);
	if (err != JsNoError) catchException();
	return err;
}
JsErrorCode CALLBACK JsSetPromiseContinuationCallbackHook(
	JsPromiseContinuationCallback promiseContinuationCallback, 
	void* callbackState)
{
	return JsSetPromiseContinuationCallback(promiseContinuationCallback, callbackState);
}

BOOL WINAPI DllMain(
	_In_ HINSTANCE hinstDLL,
	_In_ DWORD     fdwReason,
	_In_ LPVOID    lpvReserved
)
{
	if (fdwReason == DLL_PROCESS_ATTACH)
	{
		ondebug(requestDebugger());
		
		cout << "BDSX: Attached" << endl;

		g_mcf.free = (void(*)(void*)) * s_iatUcrtbase.getFunctionStore("free");
		g_mcf.malloc = (void* (*)(size_t)) * s_iatUcrtbase.getFunctionStore("malloc");
		g_mcf.load();
		Text16 commandLine = (Text16)unwide(GetCommandLineW());
		readArgument(commandLine); // exepath
		bool modulePathSetted = false;
		while (!commandLine.empty())
		{
			Text16 option = readArgument(commandLine);
			if (option == u"-M")
			{
				Text16 modulePath = readArgument(commandLine);

				if (modulePathSetted)
				{
					ConsoleColorScope _color = FOREGROUND_RED | FOREGROUND_INTENSITY;
					cerr << "BDSX: multiple modules are not supported" << endl;
					continue;
				}
				modulePathSetted = true;

				Require::init(modulePath);
			}
			else if (option == u"--mutex")
			{
				Text16 name = readArgument(commandLine);
				s_singleInstance.create(TSZ16() << u"BDSX_" << name);
			}
		}

		if (!modulePathSetted)
		{
			ConsoleColorScope _color = FOREGROUND_RED | FOREGROUND_INTENSITY;
			cerr << "BDSX: Module is not defined // -M [module path]" << endl;
		}

		g_mcf.hookOnLoopStart([](DedicatedServer* server, ServerInstance * instance) {
			g_server = instance;
		});
		g_mcf.hookOnScriptLoading([]{
			// create require
			Require::start();
		});
		s_iatWS2_32.hooking(2, bindHook);
		s_iatWS2_32.hooking(3, closesocketHook);
		s_iatWS2_32.hooking(17, recvfromHook);
		s_iatChakra.hooking("JsCreateContext", JsCreateContextHook);
		s_iatChakra.hooking("JsCreateRuntime", JsCreateRuntimeHook);
		s_iatChakra.hooking("JsDisposeRuntime", JsDisposeRuntimeHook);
		s_iatChakra.hooking("JsRunScript", JsRunScriptHook);
		s_iatChakra.hooking("JsCallFunction", JsCallFunctionHook);
		// s_iatChakra.hooking("JsSetPromiseContinuationCallback", JsSetPromiseContinuationCallbackHook);
	}
	return true;
}

