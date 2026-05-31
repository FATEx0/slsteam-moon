#pragma once

class CProtoBufMsgBase;

namespace Misc
{
	bool shouldFakeOffline();
	void recvMsg(CProtoBufMsgBase* msg);
}
