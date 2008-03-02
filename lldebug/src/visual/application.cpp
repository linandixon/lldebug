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
#include "visual/mediator.h"
#include "visual/mainframe.h"
#include "visual/application.h"
#include "visual/strutils.h"

IMPLEMENT_APP(lldebug::visual::Application)

/*int main(int argc, char *argv[]) {
	return wxEntry(argc, argv);
}*/

namespace lldebug {
namespace visual {

Application::Application()
	: m_mediator(new Mediator) {
	SetAppName(wxT("lldebug frame"));
}

Application::~Application() {
	if (m_mediator != NULL) {
		delete m_mediator;
		m_mediator = NULL;
	}
}

bool Application::OnInit() {
	std::string hostName = (this->argc >= 2
		? wxConvToUTF8(this->argv[1])
		: "localhost");
	std::string portName = (this->argc >= 3
		? wxConvToUTF8(this->argv[2])
		: "51123");

	// Start connecting.
	if (m_mediator->Initialize(hostName, portName) != 0) {
		return false;
	}

	MainFrame* frame = new MainFrame();
	SetTopWindow(frame);
	frame->Show();
	m_mediator->SetMainFrame(frame);

	wxLogWindow *log = new wxLogWindow(frame, _("lldebug logger"), true);
	wxLog::SetActiveTarget(log);
    return true;
}

int Application::OnExit() {
	return 0;
}

} // end of namespace visual
} // end of namespace lldebug
