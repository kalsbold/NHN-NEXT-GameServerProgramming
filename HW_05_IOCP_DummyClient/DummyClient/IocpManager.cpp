﻿#include "stdafx.h"
#include "Exception.h"
#include "ThreadLocal.h"
#include "IocpManager.h"
#include "ClientSession.h"
#include "SessionManager.h"
#include "DummyClient.h"

#define GQCS_TIMEOUT			20 // INFINITE
#define THREAD_QUIT_KEY		9999

IocpManager* GIocpManager = nullptr;

LPFN_CONNECTEX IocpManager::mFnConnectEx = nullptr;
LPFN_DISCONNECTEX IocpManager::mFnDisconnectEx = nullptr;

char IocpManager::mConnectBuf[64] = { 0, };


BOOL IocpManager::ConnectEx( SOCKET hSocket, const struct sockaddr *name, int nameLen, 
							 PVOID lpSendBuffer, DWORD dwSendDataLength, LPDWORD lpdwBytesSent, LPOVERLAPPED lpOverlapped )
{
	return mFnConnectEx( hSocket, name, nameLen, lpSendBuffer, dwSendDataLength, lpdwBytesSent, lpOverlapped );
}

BOOL IocpManager::DisconnectEx( SOCKET hSocket, LPOVERLAPPED lpOverlapped, DWORD dwFlags, DWORD reserved )
{
	return mFnDisconnectEx(hSocket, lpOverlapped, dwFlags, reserved);
}

IocpManager::IocpManager() : mCompletionPort(NULL), mIoThreadCount(2)
{	
}


IocpManager::~IocpManager()
{
	if ( mThreadHandle && mIoThreadCount > 0 )
	{
		for ( int i = 0; i < mIoThreadCount; ++i )
		{
			CloseHandle( mThreadHandle[i] );
		}

		delete[] mThreadHandle;
	}
}

bool IocpManager::Initialize()
{
	/// set num of I/O threads
	SYSTEM_INFO si;
	GetSystemInfo(&si);

	if ( THREAD_COUNT <= 0 || THREAD_COUNT > MAX_IO_THREAD )
	{
		mIoThreadCount = si.dwNumberOfProcessors;
	}
	else
	{
		mIoThreadCount = THREAD_COUNT;
	}
	
	/// winsock initializing
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;

	/// Create I/O Completion Port
	mCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (mCompletionPort == NULL)
		return false;

	/// create TCP socket
	mListenSocket = WSASocket( AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED );
	if ( mListenSocket == INVALID_SOCKET )
		return false;

	HANDLE handle = CreateIoCompletionPort( (HANDLE)mListenSocket, mCompletionPort, 0, 0 );
	if ( handle != mCompletionPort )
	{
		printf_s( "[DEBUG] listen socket IOCP register error: %d\n", GetLastError() );
		return false;
	}

	int opt = 1;
	setsockopt( mListenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof( int ) );

	/// bind
	SOCKADDR_IN serveraddr;
	ZeroMemory( &serveraddr, sizeof( serveraddr ) );
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_port = 0;
	serveraddr.sin_addr.s_addr = htonl( INADDR_ANY );

	if ( SOCKET_ERROR == bind( mListenSocket, (SOCKADDR*)&serveraddr, sizeof( serveraddr ) ) )
		return false;

	GUID guidDisconnectEx = WSAID_DISCONNECTEX;
	DWORD bytes = 0;
	if ( SOCKET_ERROR == WSAIoctl( mListenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidDisconnectEx, sizeof( GUID ), &mFnDisconnectEx, sizeof( LPFN_DISCONNECTEX ), &bytes, NULL, NULL ) )
		return false;

	GUID guidConnectEx = WSAID_CONNECTEX;
	if ( SOCKET_ERROR == WSAIoctl( mListenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guidConnectEx, sizeof( GUID ), &mFnConnectEx, sizeof( LPFN_CONNECTEX ), &bytes, NULL, NULL ) )
		return false;
	
	/// make session pool
	GSessionManager->PrepareSessions();

	return true;
}

bool IocpManager::StartIoThreads()
{
	/// I/O Thread
	if ( mIoThreadCount <= 0 )
	{
		return false;
	}

	mThreadHandle = new HANDLE[mIoThreadCount];

	for (int i = 0; i < mIoThreadCount; ++i)
	{
		DWORD dwThreadId;
		mThreadHandle[i] = (HANDLE)_beginthreadex( NULL, 0, IoWorkerThread, (LPVOID)( i ), 0, (unsigned int*)&dwThreadId );
		if ( mThreadHandle[i] == NULL )
			return false;
	}

	return true;
}

void IocpManager::StartConnect()
{
	DWORD startTime = timeGetTime();

	if ( TIME <= 0 || TIME > 100000 )
	{
		TIME = 60;
	}
	
	// connect
	while ( GSessionManager->ConnectSessions() && 
			timeGetTime() - startTime < static_cast<UINT>( TIME * 1000 ) )
	{
		Sleep( 100 );
	}

	for ( int i = 0; i < mIoThreadCount; ++i )
	{
		PostQueuedCompletionStatus( mCompletionPort, 0, THREAD_QUIT_KEY, NULL );
	}

	WaitForMultipleObjects( mIoThreadCount, mThreadHandle, TRUE, INFINITE );
}

void IocpManager::Finalize()
{
	CloseHandle(mCompletionPort);

	/// winsock finalizing
	WSACleanup();
}

unsigned int WINAPI IocpManager::IoWorkerThread(LPVOID lpParam)
{
	LThreadType = THREAD_IO_WORKER;
	LIoThreadId = reinterpret_cast<int>(lpParam);

	HANDLE hComletionPort = GIocpManager->GetComletionPort();
	
	while ( true )
	{
		/// IOCP 작업 돌리기
		DWORD dwTransferred = 0;
		OverlappedIOContext* context = nullptr;
		ULONG_PTR completionKey = 0;

		int ret = GetQueuedCompletionStatus(hComletionPort, &dwTransferred, (PULONG_PTR)&completionKey, (LPOVERLAPPED*)&context, GQCS_TIMEOUT);

		if ( completionKey == THREAD_QUIT_KEY )
		{
			break;
		}

		ClientSession* theClient = context ? context->mSessionObject : nullptr ;
		
		if (ret == 0 || dwTransferred == 0)
		{
			int gle = GetLastError();

			/// check time out first 
			if ( gle == WAIT_TIMEOUT && context == nullptr )
			{
				continue;
			}
		
			if (context->mIoType == IO_RECV || context->mIoType == IO_SEND )
			{
				CRASH_ASSERT(nullptr != theClient);

				/// In most cases in here: ERROR_NETNAME_DELETED(64)

				theClient->DisconnectRequest(DR_COMPLETION_ERROR);

				DeleteIoContext(context);

				continue;
			}
		}

		CRASH_ASSERT(nullptr != theClient);
	
		bool completionOk = false;
		switch (context->mIoType)
		{
		case IO_DISCONNECT:
			theClient->DisconnectCompletion(static_cast<OverlappedDisconnectContext*>(context)->mDisconnectReason);
			completionOk = true;
			break;

		case IO_CONNECT:
			theClient->ConnectCompletion();
			completionOk = true;
			break;

		case IO_RECV_ZERO:
			completionOk = PreReceiveCompletion(theClient, static_cast<OverlappedPreRecvContext*>(context), dwTransferred);
			break;

		case IO_SEND:
			completionOk = SendCompletion(theClient, static_cast<OverlappedSendContext*>(context), dwTransferred);
			break;

		case IO_RECV:
			completionOk = ReceiveCompletion(theClient, static_cast<OverlappedRecvContext*>(context), dwTransferred);
			break;

		default:
			printf_s("Unknown I/O Type: %d\n", context->mIoType);
			CRASH_ASSERT(false);
			break;
		}

		if ( !completionOk )
		{
			/// connection closing
			theClient->DisconnectRequest(DR_IO_REQUEST_ERROR);
		}

		DeleteIoContext(context);
	}

	return 0;
}

bool IocpManager::PreReceiveCompletion(ClientSession* client, OverlappedPreRecvContext* context, DWORD dwTransferred)
{
	/// real receive...
	return client->PostRecv();
}

bool IocpManager::ReceiveCompletion(ClientSession* client, OverlappedRecvContext* context, DWORD dwTransferred)
{
	client->RecvCompletion(dwTransferred);

	/// echo back
	return client->PostSend();
}

bool IocpManager::SendCompletion(ClientSession* client, OverlappedSendContext* context, DWORD dwTransferred)
{
	client->SendCompletion(dwTransferred);

	if (context->mWsaBuf.len != dwTransferred)
	{
		printf_s("Partial SendCompletion requested [%d], sent [%d]\n", context->mWsaBuf.len, dwTransferred) ;
		return false;
	}
	
	/// zero receive
	return client->PreRecv();
}
