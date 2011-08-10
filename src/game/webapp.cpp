/* Webapp class by Sushi and Redix */

#include <base\math.h>

#include <stdio.h>
#include "webapp.h"

// TODO: non-blocking
// TODO: fix client

IWebapp::IWebapp(const char* WebappIp)
{
	char aBuf[512];
	int Port = 80;
	str_copy(aBuf, WebappIp, sizeof(aBuf));

	for(int k = 0; aBuf[k]; k++)
	{
		if(aBuf[k] == ':')
		{
			Port = str_toint(aBuf+k+1);
			aBuf[k] = 0;
			break;
		}
	}

	if(net_host_lookup(aBuf, &m_Addr, NETTYPE_IPV4) != 0)
		net_host_lookup("localhost", &m_Addr, NETTYPE_IPV4);
	m_Addr.port = Port;
	
	m_Connections.delete_all();
}

bool IWebapp::SendRequest(const char *pData, int Type, IStream *pResponse, void *pUserData)
{
	CHttpConnection *pCon = new CHttpConnection();
	if(!pCon->Create(m_Addr, Type, pResponse))
		return false;
	if(!pCon->Send(pData, str_length(pData)))
	{
		pCon->Close();
		return false;
	}
	if(pUserData)
		pCon->m_pUserData = pUserData;
	m_Connections.add(pCon);
	return true;
}

int IWebapp::Update()
{
	int Size = m_Connections.size();
	int Max = 3;
	for(int i = 0; i < min(m_Connections.size(), Max); i++)
	{
		int Result = m_Connections[i]->Update();
		if(Result != 0)
		{
			if(Result == 1)
			{
				dbg_msg("webapp", "received response");
				OnResponse(m_Connections[i]->m_Type, m_Connections[i]->m_pResponse, m_Connections[i]->m_pUserData);
			}
			else
				dbg_msg("webapp", "connection error");
			
			delete m_Connections[i];
			m_Connections.remove_index_fast(i);
		}
	}
	return Size - m_Connections.size();
}

// TODO: own file
// TODO: support for chunked transfer-encoding?
// TODO: error handling
bool CHttpConnection::CHeader::Parse(char *pStr)
{
	char *pEnd = (char*)str_find(pStr, "\r\n\r\n");
	if(!pEnd)
		return false;
	
	*(pEnd+2) = 0;
	char *pData = pStr;
	
	if(sscanf(pData, "HTTP/%*d.%*d %d %*s\r\n", &this->m_StatusCode) != 1)
	{
		m_Error = true;
		return false;
	}
	
	while(sscanf(pData, "Content-Length: %ld\r\n", &this->m_ContentLength) != 1)
	{
		char *pLineEnd = (char*)str_find(pData, "\r\n");
		if(!pLineEnd)
		{
			m_Error = true;
			return false;
		}
		pData = pLineEnd + 2;
	}
	
	m_Size = (pEnd-pStr)+4;
	return true;
}

CHttpConnection::~CHttpConnection()
{
	if(m_pResponse)
		delete m_pResponse;
	Close();
}

bool CHttpConnection::Create(NETADDR Addr, int Type, IStream *pResponse)
{
	m_Socket = net_tcp_create(Addr);
	if(m_Socket.type == NETTYPE_INVALID)
		return false;
	if(net_tcp_connect(m_Socket, &Addr) != 0)
	{
		Close();
		return false;
	}
	m_Connected = true;
	
	net_set_non_blocking(m_Socket);
	
	m_Type = Type;
	m_pResponse = pResponse;
	return true;
}

void CHttpConnection::Close()
{
	net_tcp_close(m_Socket);
	m_Connected = false;
}

bool CHttpConnection::Send(const char *pData, int Size)
{
	while(m_Connected)
	{
		int Send = net_tcp_send(m_Socket, pData, Size);
		if(Send < 0)
			return false;

		if(Send >= Size)
			return true;

		pData += Send;
		Size -= Send;
	}
	return false;
}

int CHttpConnection::Update()
{
	if(!m_Connected)
		return -1;
	
	char aBuf[1024] = {0};
	int Bytes = net_tcp_recv(m_Socket, aBuf, sizeof(aBuf));
	
	if(Bytes > 0)
	{
		if(m_Header.m_Size == -1)
		{
			m_HeaderBuffer.Write(aBuf, Bytes);
			if(m_Header.Parse(m_HeaderBuffer.GetData()))
			{
				if(m_Header.m_Error)
					return -1;
				else
					m_pResponse->Write(m_HeaderBuffer.GetData()+m_Header.m_Size, m_HeaderBuffer.Size()-m_Header.m_Size);
			}
		}
		else
		{
			m_pResponse->Write(aBuf, Bytes);
		}
	}
	else if(Bytes < 0)
	{
		if(net_would_block()) // no data received
			return 0;
		
		return -1;
	}
	else
		return m_pResponse->Size() == m_Header.m_ContentLength ? 1 : -1;
	
	return 0;
}
