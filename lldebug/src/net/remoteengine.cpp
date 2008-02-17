/*
 * Copyright (c) 2005-2008  cielacanth <cielacanth AT s60.xrea.com>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "precomp.h"
#include "net/remoteengine.h"

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <sstream>
#include <fstream>

namespace lldebug {
namespace net {

using boost::asio::ip::tcp;

class SocketBase {
private:
	RemoteEngine *m_engine;
	boost::asio::ip::tcp::socket m_socket;
	bool m_isConnected;

	typedef std::queue<RemoteCommand> WriteCommandQueue;
	/// This object is handled in one specific thread.
	WriteCommandQueue m_writeCommandQueue;
	
public:
	/// Write command data.
	void Write(const RemoteCommandHeader &header, const RemoteCommandData &data) {
		RemoteCommand command(header, data);

		GetService().post(boost::bind(&SocketBase::doWrite, this, command));
	}

	/// Close this socket.
	void Close() {
		GetService().post(boost::bind(&SocketBase::doClose, this));
	}

	void Connected() {
		GetService().post(boost::bind(&SocketBase::connected, this));
	}

	/// Is this socket connected ?
	bool IsConnected() {
		return m_isConnected;
	}

	/// Get the remote engine.
	RemoteEngine *GetEngine() {
		return m_engine;
	}

	/// Get the io_service object.
	boost::asio::io_service &GetService() {
		return m_socket.io_service();
	}

	/// Get the socket object.
	boost::asio::ip::tcp::socket &GetSocket() {
		return m_socket;
	}

protected:
	SocketBase(RemoteEngine *engine, boost::asio::io_service &ioService)
		: m_engine(engine), m_socket(ioService), m_isConnected(false) {
	}

	virtual ~SocketBase() {
	}

private:
	void connected() {
		if (!m_isConnected) {
			m_isConnected = true;
			asyncReadCommand();
		}
	}

	void asyncReadCommand() {
		shared_ptr<RemoteCommand> command(new RemoteCommand);

		boost::asio::async_read(m_socket,
			boost::asio::buffer(&command->GetHeader(), sizeof(RemoteCommandHeader)),
			boost::bind(
				&SocketBase::handleReadCommand, this, command,
				boost::asio::placeholders::error));
	}

	void handleReadCommand(shared_ptr<RemoteCommand> command,
						   const boost::system::error_code &error) {
		if (!error) {
			// Check that response is OK.
			if (command->GetCtxId() < 0) {
				doClose();
			}

			// Read the command data if exists.
			if (command->GetDataSize() > 0) {
				command->ResizeData();

				boost::asio::async_read(m_socket,
					boost::asio::buffer(
						command->GetImplData(),
						command->GetDataSize()),
					boost::bind(
						&SocketBase::handleReadData, this, command,
						boost::asio::placeholders::error));
			}
			else {
				handleReadData(command, error);
			}
		}
		else {
			doClose();
		}
	}

	/// It's called after the end of the command reading.
	void handleReadData(shared_ptr<RemoteCommand> command,
						const boost::system::error_code &error) {
		if (!error) {
			assert(command != NULL);
			m_engine->HandleReadCommand(*command);

			// Prepare the new command.
			asyncReadCommand();
		}
		else {
			doClose();
		}
	}

	/// Send the asynchronous write order.
	/// The memory of the command must be kept somewhere.
	void asyncWrite(const RemoteCommand &command) {
		SaveLog(command);

		if (command.GetDataSize() == 0) {
			// Delete the command memory.
			boost::asio::async_write(m_socket,
				boost::asio::buffer(&command.GetHeader(), sizeof(RemoteCommandHeader)),
				boost::bind(
					&SocketBase::handleWrite, this, true,
					boost::asio::placeholders::error));
		}
		else {
			// Don't delete the command memory.
			boost::asio::async_write(m_socket,
				boost::asio::buffer(&command.GetHeader(), sizeof(RemoteCommandHeader)),
				boost::bind(
					&SocketBase::handleWrite, this, false,
					boost::asio::placeholders::error));

			// Write command data.
			boost::asio::async_write(m_socket,
				boost::asio::buffer(command.GetImplData(), command.GetDataSize()),
				boost::bind(
					&SocketBase::handleWrite, this, true,
					boost::asio::placeholders::error));
		}
	}

	/// Do the asynchronous writing of the command.
	/// The command memory must be kept until the end of the writing,
	/// so there is a write command queue.
	void doWrite(const RemoteCommand &command) {
		bool isProgress = !m_writeCommandQueue.empty();
		m_writeCommandQueue.push(command);

		if (!isProgress) {
			asyncWrite(m_writeCommandQueue.front());
		}
	}

	/// It's called after the end of writing command.
	/// The command memory is deleted if possible.
	void handleWrite(bool deleteCommand,
					 const boost::system::error_code& error) {
		if (!error) {
			if (deleteCommand) {
				m_writeCommandQueue.pop();

				// Begin the new write order.
				if (!m_writeCommandQueue.empty()) {
					asyncWrite(m_writeCommandQueue.front());
				}
			}
		}
		else {
			doClose();
		}
	}

	void doClose() {
		m_socket.close();
		m_isConnected = false;
	}
};


/*-----------------------------------------------------------------*/
class ContextSocket : public SocketBase {
private:
	tcp::acceptor m_acceptor;

public:
	explicit ContextSocket(RemoteEngine *engine,
						   boost::asio::io_service &ioService,
						   tcp::endpoint endpoint)
		: SocketBase(engine, ioService)
		, m_acceptor(ioService, endpoint) {
	}

	/// Start connection.
	int Start(int waitSeconds) {
		m_acceptor.async_accept(GetSocket(),
			boost::bind(
				&ContextSocket::handleAccept, this,
				boost::asio::placeholders::error));

		// Wait for connection if need.
		if (waitSeconds >= 0) {
			boost::xtime current, end;
			boost::xtime_get(&end, boost::TIME_UTC);
			end.sec += waitSeconds;

			// IsConnected become true in handleConnect.
			while (!IsConnected()) {
				boost::xtime_get(&current, boost::TIME_UTC);
				if (boost::xtime_cmp(current, end) >= 0) {
					return -1;
				}

				// Do async operation.
				GetService().poll_one();
				GetService().reset();

				current.nsec += 100 * 1000 * 1000;
				boost::thread::sleep(current);
			}
		}
		
		return 0;
	}

	virtual ~ContextSocket() {
	}

protected:
	void handleAccept(const boost::system::error_code &error) {
		if (!error) {
			this->Connected();
		}
		else {
			m_acceptor.async_accept(GetSocket(),
				boost::bind(
					&ContextSocket::handleAccept, this,
					boost::asio::placeholders::error));
		}
	}
};


/*-----------------------------------------------------------------*/
class FrameSocket : public SocketBase {
private:
	tcp::resolver::iterator m_endpointIterator;

public:
	explicit FrameSocket(RemoteEngine *engine,
						 boost::asio::io_service &ioService,
						 tcp::resolver::query query)
		: SocketBase(engine, ioService) {
		tcp::resolver resolver(ioService);
		m_endpointIterator = resolver.resolve(query);
	}

	virtual ~FrameSocket() {
	}

	/// Start connection.
	int Start(int waitSeconds) {
		tcp::resolver::iterator endpoint_iterator = m_endpointIterator;
		tcp::endpoint endpoint = *endpoint_iterator;

		GetSocket().async_connect(endpoint,
			boost::bind(
				&FrameSocket::handleConnect, this,
				boost::asio::placeholders::error, endpoint_iterator));

		// Wait for connection if need.
		if (waitSeconds >= 0) {
			boost::xtime current, end;
			boost::xtime_get(&end, boost::TIME_UTC);
			end.sec += waitSeconds;

			// IsOpen become true in handleConnect.
			while (!IsConnected()) {
				boost::xtime_get(&current, boost::TIME_UTC);
				if (boost::xtime_cmp(current, end) >= 0) {
					return -1;
				}

				// Do async operation.
				GetService().poll_one();
				GetService().reset();

				current.nsec += 100 * 1000 * 1000;
				boost::thread::sleep(current);
			}
		}
		
		return 0;
	}

protected:
	virtual void handleConnect(const boost::system::error_code &error,
							   tcp::resolver::iterator endpointIterator) {
		if (!error) {
			// The connection was successful.
			this->Connected();
		}
		else {
			//if (endpointIterator != tcp::resolver::iterator()) {
			// The connection failed. Try the next endpoint in the list.
			tcp::endpoint endpoint = *endpointIterator;

			GetSocket().close();
			GetSocket().async_connect(endpoint,
				boost::bind(
					&FrameSocket::handleConnect, this,
					boost::asio::placeholders::error, endpointIterator));
		}
		/*else {
			std::cout << "Error: " << error << "\n";
		}*/
	}
};


/*-----------------------------------------------------------------*/
RemoteEngine::RemoteEngine()
	: m_isThreadActive(false), m_commandIdCounter(0)
	, m_ctxId(-1) {
}

RemoteEngine::~RemoteEngine() {
	StopThread();
}

int RemoteEngine::StartContext(int portNum, int ctxId, int waitSeconds) {
	scoped_lock lock(m_mutex);
	shared_ptr<ContextSocket> socket;

	// limit time
	boost::xtime xt;
	boost::xtime_get(&xt, boost::TIME_UTC);
	xt.sec += waitSeconds;

	socket.reset(new ContextSocket(
		this, m_ioService,
		tcp::endpoint(tcp::v4(), portNum)));

	// Start connection.
	if (socket->Start(waitSeconds) != 0) {
		return -1;
	}

	StartThread();
	m_commandIdCounter = 1;
	m_socket = boost::shared_static_cast<SocketBase>(socket);
	DoStartConnection(ctxId);

	// Wait for START_CONNECTION and ctxId value.
	if (m_ctxId < 0) {
		m_ctxCond.timed_wait(lock, xt);

		if (m_ctxId < 0) {
			return -1;
		}
	}

	return 0;
}

int RemoteEngine::StartFrame(const std::string &hostName,
							 const std::string &portName,
							 int waitSeconds) {
	scoped_lock lock(m_mutex);
	shared_ptr<FrameSocket> socket;

	// limit time
	boost::xtime xt;
	boost::xtime_get(&xt, boost::TIME_UTC);
	xt.sec += waitSeconds;

	// Create socket object.
	socket.reset(new FrameSocket(
		this,
		m_ioService,
		tcp::resolver::query(hostName, portName)));

	// Start connection.
	if (socket->Start(waitSeconds) != 0) {
		return -1;
	}

	StartThread();
	m_commandIdCounter = 2;
	m_socket = boost::shared_static_cast<SocketBase>(socket);

	// Wait for START_CONNECTION and ctxId value.
	if (m_ctxId < 0) {
		m_ctxCond.timed_wait(lock, xt);

		if (m_ctxId < 0) {
			return -1;
		}
	}

	return 0;
}

void RemoteEngine::SetCtxId(int ctxId) {
	scoped_lock lock(m_mutex);

	if (m_ctxId != ctxId) {
		m_ctxId = ctxId;
		m_ctxCond.notify_all();
	}
}

bool RemoteEngine::IsConnected() {
	scoped_lock lock(m_mutex);

	if (m_socket == NULL) {
		return false;
	}

	return m_socket->IsConnected();
}

RemoteCommand RemoteEngine::GetCommand() {
	scoped_lock lock(m_mutex);
	return m_readCommandQueue.front();
}

void RemoteEngine::PopCommand() {
	scoped_lock lock(m_mutex);
	m_readCommandQueue.pop();
}

bool RemoteEngine::HasCommand() {
	scoped_lock lock(m_mutex);
	return !m_readCommandQueue.empty();
}

bool RemoteEngine::IsThreadActive() {
	scoped_lock lock(m_mutex);
	return m_isThreadActive;
}

void RemoteEngine::SetThreadActive(bool is) {
	scoped_lock lock(m_mutex);
	m_isThreadActive = is;
}

void RemoteEngine::DoStartConnection(int ctxId) {
	scoped_lock lock(m_mutex);
	RemoteCommandHeader header;

	header = InitCommandHeader(
		REMOTECOMMANDTYPE_START_CONNECTION,
		0, 0);
	header.ctxId = ctxId;
	m_socket->Write(header, RemoteCommandData());
}

void RemoteEngine::DoEndConnection() {
	scoped_lock lock(m_mutex);

	WriteCommand(
		REMOTECOMMANDTYPE_END_CONNECTION,
		RemoteCommandData());
}
/*
void RemoteEngine::DoEndConnection() {
	RemoteCommandHeader header;

	header.type = REMOTECOMMANDTYPE_END_CONNECTION;
	header.ctxId = m_ctxId;
	header.commandId = 0;
	header.dataSize = 0;
	HandleReadCommand(RemoteCommand(header, RemoteCommandData()));
}*/

template<class Fn1>
class binderobj {
public:
	typedef typename Fn1::result_type result_type;

	explicit binderobj(const Fn1 &func, const typename Fn1::argument_type &left)
		: m_op(func), m_value(left) {
	}

	result_type operator()() const {
		return m_op(m_value);
	}

protected:
	Fn1 m_op;	// the functor to apply
	typename Fn1::argument_type m_value;	// the left operand
};

template<class Fn1, class Ty>
inline binderobj<Fn1> bindobj(const Fn1 &op, const Ty &arg) {
	return binderobj<Fn1>(op, arg);
}

void RemoteEngine::StartThread() {
	scoped_lock lock(m_mutex);

	if (!IsThreadActive()) {
		boost::function0<void> fn = bindobj(
			std::mem_fun(&RemoteEngine::ServiceThread), this);
		m_thread.reset(new boost::thread(fn));
	}
}

void RemoteEngine::StopThread() {
	if (IsConnected()) {
		DoEndConnection();
	}

	if (IsThreadActive()) {
		SetThreadActive(false);
	}

	if (m_thread != NULL) {
		m_thread->join();
		m_thread.reset();
		m_socket.reset();
	}
}

void RemoteEngine::ServiceThread() {
	// If IsThreadActive is true, the thread has already started.
	if (IsThreadActive()) {
		return;
	}

	SetThreadActive(true);
	try {
		for (;;) {
			// 10 works are set.
			for (int i = 0; i < 10; ++i) {
				m_ioService.poll_one();
			}

			// If there is no work, exit this thread.
			if (!IsThreadActive() && m_ioService.poll_one() == 0) {
				break;
			}

			m_ioService.reset();
			boost::xtime xt;
			boost::xtime_get(&xt, boost::TIME_UTC);
			xt.nsec += 10 * 1000 * 1000; // 1ms = 1000 * 1000nsec
			boost::thread::sleep(xt);
		}
	}
	catch (std::exception &ex) {
		printf("%s\n", ex.what());
	}

	SetThreadActive(false);
}

void RemoteEngine::HandleReadCommand(const RemoteCommand &command_) {
	scoped_lock lock(m_mutex);
	RemoteCommand command = command_;

	// First, find a response command.
	WaitResponseCommandList::iterator it = m_waitResponseCommandList.begin();
	while (it != m_waitResponseCommandList.end()) {
		const RemoteCommandHeader &header_ = (*it).header;

		if (command.GetCtxId() == header_.ctxId
			&& command.GetCommandId() == header_.commandId) {
			RemoteCommandCallback response = (*it).response;
			it = m_waitResponseCommandList.erase(it);

			command.SetResponse(response);
			break;
		}
		else {
			++it;
		}
	}

	switch (command.GetType()) {
	case REMOTECOMMANDTYPE_START_CONNECTION:
		SetCtxId(command.GetCtxId());
		break;
	case REMOTECOMMANDTYPE_END_CONNECTION:
		SetCtxId(-1);
		break;
	default:
		if (m_ctxId < 0) {
			SetCtxId(command.GetCtxId());
		}
		break;
	}

	m_readCommandQueue.push(command);
}

RemoteCommandHeader RemoteEngine::InitCommandHeader(RemoteCommandType type,
											  size_t dataSize,
											  int commandId) {
	scoped_lock lock(m_mutex);
	RemoteCommandHeader header;

	header.type = type;
	header.ctxId = m_ctxId;
	header.dataSize = (boost::uint32_t)dataSize;

	// Set a new value to header.commandId if commandId == 0
	if (commandId == 0) {
		header.commandId = m_commandIdCounter;
		m_commandIdCounter += 2;
	}
	else {
		header.commandId = commandId;
	}

	return header;
}

void RemoteEngine::WriteCommand(RemoteCommandType type,
								const RemoteCommandData &data) {
	scoped_lock lock(m_mutex);
	RemoteCommandHeader header;

	header = InitCommandHeader(type, data.GetSize());
	m_socket->Write(header, data);
}

void RemoteEngine::WriteCommand(RemoteCommandType type,
								const RemoteCommandData &data,
								const RemoteCommandCallback &response) {
	scoped_lock lock(m_mutex);
	WaitResponseCommand wcommand;

	wcommand.header = InitCommandHeader(type, data.GetSize());
	wcommand.response = response;
	m_socket->Write(wcommand.header, data);
	m_waitResponseCommandList.push_back(wcommand);
}

void RemoteEngine::WriteResponse(const RemoteCommand &readCommand,
								 RemoteCommandType type,
								 const RemoteCommandData &data) {
	scoped_lock lock(m_mutex);
	RemoteCommandHeader header;

	header = InitCommandHeader(
		type,
		data.GetSize(),
		readCommand.GetCommandId());
	m_socket->Write(header, data);
}

void RemoteEngine::ResponseSuccessed(const RemoteCommand &command) {
	WriteResponse(command, REMOTECOMMANDTYPE_SUCCESSED, RemoteCommandData());
}

void RemoteEngine::ResponseFailed(const RemoteCommand &command) {
	WriteResponse(command, REMOTECOMMANDTYPE_FAILED, RemoteCommandData());
}

void RemoteEngine::ChangedState(bool isBreak) {
	RemoteCommandData data;

	data.Set_ChangedState(isBreak);
	WriteCommand(
		REMOTECOMMANDTYPE_CHANGED_STATE,
		data);
}

void RemoteEngine::UpdateSource(const std::string &key, int line, int updateSourceCount, const RemoteCommandCallback &response) {
	RemoteCommandData data;

	data.Set_UpdateSource(key, line, updateSourceCount);
	WriteCommand(
		REMOTECOMMANDTYPE_UPDATE_SOURCE,
		data,
		response);
}

void RemoteEngine::ForceUpdateSource() {
	WriteCommand(
		REMOTECOMMANDTYPE_FORCE_UPDATESOURCE,
		RemoteCommandData());
}

void RemoteEngine::AddedSource(const Source &source) {
	RemoteCommandData data;

	data.Set_AddedSource(source);
	WriteCommand(
		REMOTECOMMANDTYPE_ADDED_SOURCE,
		data);
}

void RemoteEngine::SaveSource(const std::string &key,
							  const string_array &sources) {
	RemoteCommandData data;

	data.Set_SaveSource(key, sources);
	WriteCommand(
		REMOTECOMMANDTYPE_SAVE_SOURCE,
		data);
}

void RemoteEngine::SetUpdateCount(int updateCount) {
	RemoteCommandData data;

	data.Set_SetUpdateCount(updateCount);
	WriteCommand(
		REMOTECOMMANDTYPE_SET_UPDATECOUNT,
		data);
}

/// Notify that the breakpoint was set.
void RemoteEngine::SetBreakpoint(const Breakpoint &bp) {
	RemoteCommandData data;

	data.Set_SetBreakpoint(bp);
	WriteCommand(
		REMOTECOMMANDTYPE_SET_BREAKPOINT,
		data);
}

void RemoteEngine::RemoveBreakpoint(const Breakpoint &bp) {
	RemoteCommandData data;

	data.Set_RemoveBreakpoint(bp);
	WriteCommand(
		REMOTECOMMANDTYPE_REMOVE_BREAKPOINT,
		data);
}

void RemoteEngine::ChangedBreakpointList(const BreakpointList &bps) {
	RemoteCommandData data;

	data.Set_ChangedBreakpointList(bps);
	WriteCommand(
		REMOTECOMMANDTYPE_CHANGED_BREAKPOINTLIST,
		data);
}

void RemoteEngine::Break() {
	WriteCommand(
		REMOTECOMMANDTYPE_BREAK,
		RemoteCommandData());
}

void RemoteEngine::Resume() {
	WriteCommand(
		REMOTECOMMANDTYPE_RESUME,
		RemoteCommandData());
}

void RemoteEngine::StepInto() {
	WriteCommand(
		REMOTECOMMANDTYPE_STEPINTO,
		RemoteCommandData());
}

void RemoteEngine::StepOver() {
	WriteCommand(
		REMOTECOMMANDTYPE_STEPOVER,
		RemoteCommandData());
}

void RemoteEngine::StepReturn() {
	WriteCommand(
		REMOTECOMMANDTYPE_STEPRETURN,
		RemoteCommandData());
}

void RemoteEngine::OutputLog(LogType type, const std::string &str, const std::string &key, int line) {
	RemoteCommandData data;

	data.Set_OutputLog(type, str, key, line);
	WriteCommand(
		REMOTECOMMANDTYPE_OUTPUT_LOG,
		data);
}

/**
 * @brief Handle the response string.
 */
struct StringResponseHandler {
	StringCallback m_callback;

	explicit StringResponseHandler(const StringCallback &callback)
		: m_callback(callback) {
	}

	void operator()(const RemoteCommand &command) {
		std::string str;
		command.GetData().Get_ValueString(str);
		m_callback(command, str);
	}
};

void RemoteEngine::Eval(const std::string &str,
						const LuaStackFrame &stackFrame,
						const StringCallback &callback) {
	RemoteCommandData data;

	data.Set_Eval(str, stackFrame);
	WriteCommand(
		REMOTECOMMANDTYPE_EVAL,
		data,
		StringResponseHandler(callback));
}

/**
 * @brief Handle the response VarList.
 */
struct VarListResponseHandler {
	LuaVarListCallback m_callback;

	explicit VarListResponseHandler(const LuaVarListCallback &callback)
		: m_callback(callback) {
	}

	void operator()(const RemoteCommand &command) {
		LuaVarList vars;
		command.GetData().Get_ValueVarList(vars);
		m_callback(command, vars);
	}
};

void RemoteEngine::RequestFieldsVarList(const LuaVar &var,
										const LuaVarListCallback &callback) {
	RemoteCommandData data;

	data.Set_RequestFieldVarList(var);
	WriteCommand(
		REMOTECOMMANDTYPE_REQUEST_FIELDSVARLIST,
		data,
		VarListResponseHandler(callback));
}

void RemoteEngine::RequestLocalVarList(const LuaStackFrame &stackFrame,
									   const LuaVarListCallback &callback) {
	RemoteCommandData data;

	data.Set_RequestLocalVarList(stackFrame);
	WriteCommand(
		REMOTECOMMANDTYPE_REQUEST_LOCALVARLIST,
		data,
		VarListResponseHandler(callback));
}

void RemoteEngine::RequestEnvironVarList(const LuaStackFrame &stackFrame,
										 const LuaVarListCallback &callback) {
	RemoteCommandData data;

	data.Set_RequestLocalVarList(stackFrame);
	WriteCommand(
		REMOTECOMMANDTYPE_REQUEST_ENVIRONVARLIST,
		data,
		VarListResponseHandler(callback));
}

void RemoteEngine::RequestEvalVarList(const string_array &array,
									  const LuaStackFrame &stackFrame,
									  const LuaVarListCallback &callback) {
	RemoteCommandData data;

	data.Set_RequestEvalVarList(array, stackFrame);
	WriteCommand(
		REMOTECOMMANDTYPE_REQUEST_EVALVARLIST,
		data,
		VarListResponseHandler(callback));
}

void RemoteEngine::RequestGlobalVarList(const LuaVarListCallback &callback) {
	WriteCommand(
		REMOTECOMMANDTYPE_REQUEST_GLOBALVARLIST,
		RemoteCommandData(),
		VarListResponseHandler(callback));
}

void RemoteEngine::RequestRegistryVarList(const LuaVarListCallback &callback) {
	WriteCommand(
		REMOTECOMMANDTYPE_REQUEST_REGISTRYVARLIST,
		RemoteCommandData(),
		VarListResponseHandler(callback));
}

void RemoteEngine::RequestStackList(const LuaVarListCallback &callback) {
	WriteCommand(
		REMOTECOMMANDTYPE_REQUEST_STACKLIST,
		RemoteCommandData(),
		VarListResponseHandler(callback));
}

void RemoteEngine::ResponseString(const RemoteCommand &command, const std::string &str) {
	RemoteCommandData data;

	data.Set_ValueString(str);
	WriteResponse(
		command,
		REMOTECOMMANDTYPE_VALUE_STRING,
		data);
}

void RemoteEngine::ResponseVarList(const RemoteCommand &command, const LuaVarList &vars) {
	RemoteCommandData data;

	data.Set_ValueVarList(vars);
	WriteResponse(
		command,
		REMOTECOMMANDTYPE_VALUE_VARLIST,
		data);
}

void RemoteEngine::ResponseBacktraceList(const RemoteCommand &command, const LuaBacktraceList &backtraces) {
	RemoteCommandData data;

	data.Set_ValueBacktraceList(backtraces);
	WriteResponse(
		command,
		REMOTECOMMANDTYPE_VALUE_BACKTRACELIST,
		data);
}

} // end of namespace net
} // end of namespace lldebug
