#include "../StdAfx.h"
#include "slot.h"
#include "PKCS11Functions.h"
#include "../PCSC/token.h"

#include "CardTemplate.h"
#include "../util/util.h"
#include "../util/syncroevent.h"
#include <mutex>

static char *szCompiledFile=__FILE__;
//extern CSyncroMutex p11EventMutex;
extern std::mutex p11Mutex;
extern auto_reset_event p11slotEvent;
extern bool bP11Terminate;
extern bool bP11Initialized;

namespace p11 {

DWORD CSlot::dwSlotCnt=1;
SlotMap CSlot::g_mSlots;
std::thread CSlot::Thread;
CCardContext *CSlot::ThreadContext=NULL;
bool CSlot::bMonitorUpdate=false;
static char szMutexName[200]="p11mutex_";

char *CSlot::mutexName(const char *szName) {
	int dwNameSize=(int)strnlen(szName,181);
	if (dwNameSize>180) dwNameSize=180;
	strncpy_s(szMutexName+9,190,szName,dwNameSize);
	szMutexName[dwNameSize+9]=0;
	return(szMutexName);
}

CSlot::CSlot(const char *szReader) {
	szName=szReader;
	lastEvent=SE_NoEvent;
	bUpdated=0;
	User=CKU_NOBODY;
	dwP11ObjCnt=1;
	dwSessionCount=0;
	pTemplate=NULL;
	//slotMutex.Create(mutexName(szReader));
	pSerialTemplate=NULL;
	hCard = NULL;
}

CSlot::~CSlot() {
	Final();
}

RESULT CSlot::GetNewSlotID(CK_SLOT_ID *pSlotID) {
	init_func
	*pSlotID=dwSlotCnt;
	dwSlotCnt++;
	_return(OK)
	exit_func
	_return(FAIL)
}

static DWORD slotMonitor(SlotMap *pSlotMap)
{
	while (true) {
		CCardContext Context;
		CSlot::ThreadContext = &Context;
		size_t dwSlotNum = pSlotMap->size();
		std::vector<SCARD_READERSTATE> state(dwSlotNum);
		std::vector<std::shared_ptr<CSlot>> slot(dwSlotNum);
		size_t i = 0;
		DWORD ris;
		{
			std::unique_lock<std::mutex> lock(p11Mutex);
			for (SlotMap::const_iterator it = pSlotMap->begin(); it != pSlotMap->end(); it++, i++) {
				if (!bP11Initialized) {
					CSlot::ThreadContext = NULL;
					return 0;
				}

				state[i].szReader = it->second->szName.c_str();
				slot[i] = it->second;
				if (ris = SCardGetStatusChange(Context, 0, &state[i], 1) != S_OK) {
					if (ris != SCARD_E_TIMEOUT) {
						Log.write("Errore nella SCardGetStatusChange - %08X", ris);
						// non uso la ExitThread!!!
						// altrimenti non chiamo i distruttori, e mi rimane tutto appeso
						// SOPRATTUTTO il p11Mutex
						CSlot::ThreadContext = NULL;
						return 1;
					}
				}
				state[i].dwCurrentState = state[i].dwEventState & (~SCARD_STATE_CHANGED);
			}
		}
		CSlot::bMonitorUpdate = false;
		while (true) {
			Context.validate();
			ris = SCardGetStatusChange(Context, 1000, state.data(), (DWORD)dwSlotNum);
			if (ris != S_OK) {
				if (CSlot::bMonitorUpdate || ris == SCARD_E_SYSTEM_CANCELLED || ris == SCARD_E_SERVICE_STOPPED || ris == SCARD_E_INVALID_HANDLE || ris == ERROR_INVALID_HANDLE) {
					Log.write("Monitor Update");
					break;
				}
				if (ris == SCARD_E_CANCELLED || bP11Terminate || !bP11Initialized) {
					Log.write("Terminate");
					p11slotEvent.set();
					CSlot::ThreadContext = NULL;
					// no exitThread, vedi sopra;
					return 0;
				}
				if (ris != SCARD_E_TIMEOUT && ris != SCARD_E_NO_READERS_AVAILABLE) {
					Log.write("Errore nella SCardGetStatusChange - %08X", ris);
					p11slotEvent.set();
					CSlot::ThreadContext = NULL;
					// no exitThread, vedi sopra;
					return 1;
				}
			}
			for (size_t i = 0; i < dwSlotNum; i++) {
				if ((state[i].dwCurrentState & SCARD_STATE_PRESENT) &&
					((state[i].dwEventState & SCARD_STATE_EMPTY) ||
					(state[i].dwEventState & SCARD_STATE_UNAVAILABLE))) {
					// una carta � stata estratta!!
					// sincronizzo sul mutex principale p11
					// le funzioni attualmente in esecuzione che vanno sulla
					// carta falliranno miseramente, ma se levi la carta
					// mentre sto firmado mica � colpa mia!

					std::unique_lock<std::mutex> lock(p11Mutex);

					slot[i]->lastEvent = SE_Removed;
					slot[i]->Final();
					slot[i]->baATR.clear();
					p11slotEvent.set();
				}
				if (((state[i].dwCurrentState & SCARD_STATE_UNAVAILABLE) ||
					(state[i].dwCurrentState & SCARD_STATE_EMPTY)) &&
					(state[i].dwEventState & SCARD_STATE_PRESENT)) {
					// una carta � stata inserita!!
					std::unique_lock<std::mutex> lock(p11Mutex);

					slot[i]->lastEvent = SE_Inserted;
					ByteArray ba;
					slot[i]->GetATR(ba);
					p11slotEvent.set();
				}
				state[i].dwCurrentState = state[i].dwEventState & (~SCARD_STATE_CHANGED);
			}
			if (ris == SCARD_E_NO_READERS_AVAILABLE) {
				Log.write("Nessun lettore connesso - %08X", ris);
				CSlot::ThreadContext = NULL;
				// no exitThread, vedi sopra;
				return 1;
			}
		}
		CSlot::ThreadContext = NULL;
	}
	// no exitThread, vedi sopra;
	return 0;
}

RESULT CSlot::AddSlot(std::shared_ptr<CSlot> pSlot,CK_SLOT_ID *pSlotID)
{
	init_func
	P11ER_CALL(GetNewSlotID(&pSlot->hSlot),
		ERR_GET_NEW_SLOT)

	*pSlotID=pSlot->hSlot;

	g_mSlots.insert(std::make_pair(pSlot->hSlot,std::move(pSlot)));
	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::DeleteSlot(CK_SLOT_ID hSlotId)
{
	init_func
	std::shared_ptr<CSlot> pSlot;
	P11ER_CALL(GetSlotFromID(hSlotId, pSlot),
		ERR_CANT_GET_SLOT);
 
	if (!pSlot) _return(CKR_SLOT_ID_INVALID);

	pSlot->CloseAllSessions();

	g_mSlots.erase(hSlotId);
	pSlot->Final();
	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::GetSlotFromReaderName(const char *name,std::shared_ptr<CSlot>&ppSlot)
{
	init_func
	for (SlotMap::iterator it=g_mSlots.begin();it!=g_mSlots.end();it++) {
		if (strcmp(it->second->szName.c_str(), name) == 0) {
			ppSlot=it->second;
			_return(OK)
		}
	}
	ppSlot=nullptr;

	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::GetSlotFromID(CK_SLOT_ID hSlotId,std::shared_ptr<CSlot>&ppSlot)
{
	init_func
	SlotMap::const_iterator pPair;
	pPair=g_mSlots.find(hSlotId);
	if (pPair==g_mSlots.end()) {
		ppSlot=nullptr;
		_return(OK)
	}
	ppSlot=pPair->second;
	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::DeleteSlotList()
{
	init_func

	int i=0;

	if (Thread.joinable())
		Thread.join();

	// TODO: verificare se � il caso di usare un thread con join a tempo

	/*while (i<5) {
		if (Thread.join(1000)==OK)
			break;
		i=i+1;
		if (CSlot::ThreadContext!=NULL)
			SCardCancel(*CSlot::ThreadContext);
	}
	if (i==5) {
		Thread.terminateThread();
	}*/

	SlotMap::iterator it=CSlot::g_mSlots.begin();
	while (it!=CSlot::g_mSlots.end()) { 
		DeleteSlot(it->second->hSlot);
		it=CSlot::g_mSlots.begin();
	}
	
	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::InitSlotList()
{
	// la InitSlotList deve aggiornare la liste degli slot;
	// cio�, deve aggiungere gli slot che non c'erano prima e
	// cancellare quelli che non ci sono pi�
	init_func
	bool bMapChanged=false;
	DWORD readersLen=0;

	CCardContext Context;

	if (!bP11Initialized)
		return 0;

	DWORD ris=SCardListReaders(Context,NULL,NULL,&readersLen);
	if (ris!=S_OK) {
		if (ris==SCARD_E_NO_READERS_AVAILABLE)
			_return(OK)
		_return(FAIL)
	}
	std::string readers;
	readers.resize(readersLen + 1);
	WIN_R_CALL(SCardListReaders(Context,NULL,&readers[0],&readersLen),SCARD_S_SUCCESS)

	const char *szReaderName=readers.c_str();

	while (*szReaderName!=0) {
		if (!bP11Initialized)
			return 0;

		// vediamo questo slot c'era gi� prima
		Log.write("reader:%s",szReaderName);
		std::shared_ptr<CSlot> pSlot;
		GetSlotFromReaderName(szReaderName, pSlot);
		if (!pSlot) {
			auto pSlot = std::make_shared<CSlot>(szReaderName);
			CK_SLOT_ID hSlotID;
			AddSlot(pSlot, &hSlotID);
			bMapChanged=true;
		}
		szReaderName = szReaderName + strnlen(szReaderName, readersLen) + 1;
	}
	// adesso vedo se tutti gli slot nella mappa ci sono ancora
	for (SlotMap::iterator it=g_mSlots.begin();it!=g_mSlots.end();it++) {
		if (!bP11Initialized)
			return 0;

		Log.write("%s", it->second->szName.c_str());
		const char *name = it->second->szName.c_str();

		const char *szReaderName = readers.c_str();
		//char *szReaderName=szReaderAlloc;
		bool bFound=false;
		while (*szReaderName!=0) {
			if (strcmp(name,szReaderName)==0) {
				bFound=true;
				break;
			}
			szReaderName = szReaderName + strnlen(szReaderName, readersLen) + 1;
		}
		if (!bFound) {
			CK_SLOT_ID ID=it->second->hSlot;
			it--;
			DeleteSlot(ID);
			bMapChanged=true;
		}
	}
	bMonitorUpdate=bMapChanged;

	if (!bP11Initialized)
		return 0;

	if (!Thread.joinable())
		Thread=std::thread(slotMonitor, &g_mSlots);

	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::IsTokenPresent(bool *bPresent)
{
	init_func
	SCARD_READERSTATE state;
	memset(&state,0,sizeof(SCARD_READERSTATE));
	state.szReader=szName.c_str();

	Context.validate();
	bool retry=false;
	while(true) {
		DWORD ris=SCardGetStatusChange(Context,0,&state,1);
		if (ris==S_OK) {
			if ((state.dwEventState & SCARD_STATE_UNAVAILABLE)==SCARD_STATE_UNAVAILABLE)
				_return(CKR_DEVICE_REMOVED)
			if ((state.dwEventState & SCARD_STATE_PRESENT)==SCARD_STATE_PRESENT)
				*bPresent=true;
			else
				*bPresent=false;
			_return(OK)
		}
		else {
			if (ris==SCARD_E_SERVICE_STOPPED || ris==SCARD_E_INVALID_HANDLE || ris==ERROR_INVALID_HANDLE) {
				// devo prendere un nuovo context e riprovare
				if (!retry)
					retry=true;
				else
					_return(FAIL)
				P11ER_CALL(Context.renew(), ERR_CANT_ESTABLISH_CONTEXT)
				continue;
			}
			if (ris==SCARD_E_NO_READERS_AVAILABLE)
				_return(CKR_DEVICE_REMOVED)
			_return(FAIL)
		}
	}

	exit_func
	_return(FAIL)
}

CK_RV CSlot::GetInfo(CK_SLOT_INFO_PTR pInfo)
{
	init_func
	pInfo->flags=CKF_REMOVABLE_DEVICE|CKF_HW_SLOT;
	// verifico che ci sia una carta inserita
	bool bPresent=false;
	P11ER_CALL(IsTokenPresent(&bPresent),
		ERR_TOKEN_PRESENT)

	if (bPresent)
		pInfo->flags |= CKF_TOKEN_PRESENT;

	memset(pInfo->slotDescription,' ',64);
	size_t SDLen=min(64,szName.size()-1);
	memcpy_s(pInfo->slotDescription,64,szName.c_str(),SDLen);

	memset(pInfo->manufacturerID,' ',32);
	// non so esattamente perch�, ma nella R1 il manufacturerID sono i primi 32 dello slotDescription
	size_t MIDLen = min(32, szName.size());
	memcpy_s(pInfo->manufacturerID, 32, szName.c_str(), MIDLen);

	pInfo->hardwareVersion.major = 0;
	pInfo->hardwareVersion.minor = 0;

	pInfo->firmwareVersion.major = 0; 
	pInfo->firmwareVersion.minor = 0; 	

	_return(OK)
	exit_func
	_return(FAIL)
}

CK_RV CSlot::GetTokenInfo(CK_TOKEN_INFO_PTR pInfo)
{
	init_func

	//CToken token;
	//if (token.Connect(szName.stringlock())) _return(CKR_TOKEN_NOT_PRESENT)
	if (!pTemplate) {
		P11ER_CALL(CCardTemplate::GetTemplate(*this,pTemplate),
			ERR_GET_TEMPLATE)
	}

	if (!pTemplate)
		_return(CKR_TOKEN_NOT_RECOGNIZED)
	
	memset(pInfo->label,' ', sizeof(pInfo->label));
	memcpy_s((char*)pInfo->label, 32, pTemplate->szName.c_str(), min(pTemplate->szName.length(), sizeof(pInfo->label)));
	memset(pInfo->manufacturerID, ' ', sizeof(pInfo->manufacturerID));

	char *manifacturer;
	size_t position;
	if (baATR.indexOf(baNXP_ATR,position))
		manifacturer = "NXP";
	else if ((baATR.indexOf(baGemalto_ATR, position)) ||
		(baATR.indexOf(baGemalto2_ATR, position)))
		manifacturer = "Gemalto";
	else
		throw CStringException("CIE non riconosciuta");

	memcpy_s((char*)pInfo->manufacturerID, 32, manifacturer, strlen(manifacturer));
	
	if (baSerial.isEmpty() || pSerialTemplate!=pTemplate) {
		pSerialTemplate=pTemplate;
		P11ER_CALL(pTemplate->FunctionList.templateGetSerial(*this,baSerial),
			ERR_READ_SERIAL)
	}

	std::string model;
	P11ER_CALL(pTemplate->FunctionList.templateGetModel(*this,model),
		ERR_READ_MODEL)

	memset(pInfo->serialNumber,' ',sizeof(pInfo->serialNumber));
	size_t UIDsize = min(sizeof(pInfo->serialNumber), baSerial.size());
	memcpy_s(pInfo->serialNumber,16,baSerial.data(),UIDsize);

	memcpy_s((char*)pInfo->label + pTemplate->szName.length() + 1, sizeof(pInfo->label) - pTemplate->szName.length() - 1, baSerial.data(), baSerial.size());

	memset(pInfo->model,' ',sizeof(pInfo->model));
	memcpy_s(pInfo->model,16,model.c_str(),min(model.length(),sizeof(pInfo->model)));	

	DWORD dwFlags;
	P11ER_CALL(pTemplate->FunctionList.templateGetTokenFlags(*this,dwFlags),
		ERR_READ_SERIAL)
	pInfo->flags=dwFlags;

	pInfo->ulTotalPublicMemory=CK_UNAVAILABLE_INFORMATION;
	pInfo->ulTotalPrivateMemory=CK_UNAVAILABLE_INFORMATION;
	pInfo->ulFreePublicMemory=CK_UNAVAILABLE_INFORMATION;
	pInfo->ulFreePrivateMemory=CK_UNAVAILABLE_INFORMATION;
	pInfo->ulMaxSessionCount = MAXSESSIONS;
	DWORD dwSessCount=0;
	SessionCount(dwSessCount);

	pInfo->ulSessionCount = dwSessCount;
	DWORD dwRWSessCount=0;
	RWSessionCount(dwRWSessCount);

	pInfo->ulRwSessionCount = dwRWSessCount;
	pInfo->ulMaxRwSessionCount = MAXSESSIONS; 

	pInfo->ulMinPinLen=5;
	pInfo->ulMaxPinLen=8;

	pInfo->hardwareVersion.major=0;
	pInfo->hardwareVersion.minor=0;

	pInfo->firmwareVersion.major=0;
	pInfo->firmwareVersion.minor=0;

	memcpy_s((char*)pInfo->utcTime,16,"1234567890123456",16);  // OK

	_return(OK)
	exit_func
	_return(FAIL)

}

CK_RV CSlot::CloseAllSessions()
{
	init_func

	SessionMap::iterator it=CSession::g_mSessions.begin();
	while (it!=CSession::g_mSessions.end()) {
		if (it->second->pSlot.get()==this) 
		{
			CSession *pSession=it->second.get();
			it++;
			CSession::DeleteSession(pSession->hSessionHandle);
		}
		else it++;
	}

	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::Init()
{
	init_func
	if (!bUpdated) {

		if (!pTemplate) {
			P11ER_CALL(CCardTemplate::GetTemplate(*this,pTemplate),
				ERR_GET_TEMPLATE)
		}

		if (!pTemplate)
			_return(CKR_TOKEN_NOT_RECOGNIZED)

		P11ER_CALL(pTemplate->FunctionList.templateInitCard(pTemplateData,*this),
			ERR_CANT_INITIALIZE_CARD_STRUCTURES)
		bUpdated=true;
	}
	_return(OK)
	exit_func
	_return(FAIL)
}

void CSlot::Final()
{
	if (bUpdated) {
		// cancello i dati del template
		pTemplate->FunctionList.templateFinalCard(pTemplateData);
		pTemplate=NULL;

		baATR.clear();
		baSerial.clear();

		P11Objects.clear();
		
		// cancello tutte le sessioni
		SessionMap::iterator it=CSession::g_mSessions.begin();
		while (it!=CSession::g_mSessions.end()) {
			if (it->second->pSlot.get()==this) 
			{
				it=CSession::g_mSessions.erase(it);
				dwSessionCount--;
			}
			else it++;
		}
		// dwSessionCount dovrebbe essere gi� a 0...
		// ma per sicurezza lo setto a manina

		User=CKU_NOBODY;
		dwSessionCount=0;
		bUpdated=false;
	}
}

RESULT CSlot::FindP11Object(CK_OBJECT_CLASS objClass,CK_ATTRIBUTE_TYPE attr,CK_BYTE *val,int valLen,std::shared_ptr<CP11Object>&pObject)
{
	init_func
	pObject=nullptr;
	for(DWORD i=0;i<P11Objects.size();i++)
	{
		auto obj=P11Objects[i];
		if (obj->ObjClass==objClass) {
			ByteArray *attrVal=NULL;
			obj->getAttribute(attr, attrVal);
			if (attrVal && attrVal->size()==valLen) {
				if (memcmp(attrVal->data(),val,valLen)==0) {
					pObject=obj;
					_return(OK)
				}
			}
		}
	}
	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::AddP11Object(std::shared_ptr<CP11Object> p11obj)
{
	init_func
	p11obj->pSlot = this;
	P11Objects.emplace_back(std::move(p11obj));
	_return(OK)
	exit_func
	_return(NULL)
}

RESULT CSlot::ClearP11Objects()
{
	init_func
	P11Objects.clear();
	ObjP11Map.clear();
	HandleP11Map.clear();
	_return(OK)
	exit_func
	_return(NULL)
}

RESULT CSlot::DelP11Object(const std::shared_ptr<CP11Object>& object)
{
	init_func
	bool bFound=false;
	for(P11ObjectVector::iterator it=P11Objects.begin();it!=P11Objects.end();it++) {
		if (*it==object) {
			bFound=true;
			P11Objects.erase(it);
			break;
		}
	}
	ER_ASSERT(bFound,ERR_FIND_OBJECT)

	ObjHandleMap::iterator itObj=ObjP11Map.find(object);
	if (itObj!=ObjP11Map.end()) {
		HandleObjMap::iterator itHandle=HandleP11Map.find(itObj->second);
		ObjP11Map.erase(itObj);
		if (itHandle!=HandleP11Map.end()) 
			HandleP11Map.erase(itHandle);
	}

	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::SessionCount(DWORD &dwSessCount)
{
	init_func
	dwSessCount=dwSessionCount;
	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::RWSessionCount(DWORD &dwRWSessCount)
{
	init_func
	dwRWSessCount=0;
	for(SessionMap::iterator it=CSession::g_mSessions.begin();it!=CSession::g_mSessions.end();it++) {
		if (it->second->pSlot.get()==this && (it->second->flags & CKF_RW_SESSION)!=0)
			dwRWSessCount++;
	}
	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::GetIDFromObject(const std::shared_ptr<CP11Object>&pObject,CK_OBJECT_HANDLE &hObject)
{
	init_func

	// controllo se l'oggetto � privato
	bool bPrivate=false;
	P11ER_CALL(pObject->IsPrivate(bPrivate),
		ERR_GET_PRIVATE)

	if (bPrivate && User!=CKU_USER)
		_return(CKR_USER_NOT_LOGGED_IN)

	ObjHandleMap::const_iterator pPair;
	pPair=ObjP11Map.find(pObject);
	if (pPair==ObjP11Map.end()) {
		// non ho trovato l'oggetto nella mappa degli oggetti;
		// devo aggiungerlo

		P11ER_CALL(GetNewObjectID(hObject),
			ERR_GET_NEW_OBJECT)

		ObjP11Map[pObject]=hObject;
		HandleP11Map[hObject]=pObject;
		_return(OK)
	}
	hObject=pPair->second;
	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::GetNewObjectID(CK_OBJECT_HANDLE &hObject) {
	init_func	

	hObject=dwP11ObjCnt;
	dwP11ObjCnt++;
	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::DelObjectHandle(const std::shared_ptr<CP11Object>& pObject)
{
	init_func
	ObjHandleMap::iterator pPair;
	pPair=ObjP11Map.find(pObject);
	if (pPair!=ObjP11Map.end()) {
		HandleObjMap::iterator pPair2;
		pPair2=HandleP11Map.find(pPair->second);
		if (pPair2!=HandleP11Map.end())
			HandleP11Map.erase(pPair2);
		ObjP11Map.erase(pPair);
	}
	
	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::GetObjectFromID(CK_OBJECT_HANDLE hObjectHandle,std::shared_ptr<CP11Object>&pObject)
{
	init_func
	pObject=nullptr;
	HandleObjMap::const_iterator pPair;
	pPair=HandleP11Map.find(hObjectHandle);
	if (pPair==HandleP11Map.end()) {
		pObject=nullptr;
		_return(OK)
	}
	pObject=pPair->second;
	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::Connect() {
	init_func
		DWORD dwProtocol;

	Context.validate();
	bool retry = false;
	while (true) {
		DWORD ris = SCardConnect(Context, szName.c_str(), SCARD_SHARE_SHARED, SCARD_PROTOCOL_T1, &hCard, &dwProtocol);
		if (ris == OK) {
			_return(OK)
		}
		else {
			if (ris == SCARD_E_SERVICE_STOPPED || ris == SCARD_E_INVALID_HANDLE || ris == ERROR_INVALID_HANDLE) {
				if (!retry)
					retry = true;
				else {
					throw CStringException(ERR_CANT_ESTABLISH_CONTEXT);
				}
				P11ER_CALL(Context.renew(),
					ERR_CANT_ESTABLISH_CONTEXT)
			}
			else {
				WIN_R_CALL(ris,SCARD_S_SUCCESS);
			}
		}
	}

	_return(OK)
		exit_func
		_return(FAIL)
}

RESULT CSlot::GetATR(ByteDynArray &ATR)
{
	init_func

	SCARD_READERSTATE state;
	state.szReader = this->szName.data();
	SCardGetStatusChange(CSlot::Context, 0, &state, 1);
	if (state.cbAtr > 0) {
		ATR = ByteArray(state.rgbAtr, state.cbAtr);
		Log.write("ATR Letto:");
		Log.writeBinData(ATR.data(), ATR.size());
	}
	else {
		ATR.clear();
		Log.write("ATR Letto: -nessuna carta inserita-");
	}

	_return(OK)
	exit_func
	_return(FAIL)
}

RESULT CSlot::GetATR(ByteArray &ATR) {
	init_func
	if (baATR.size()!=0) {
		ATR=baATR;
		_return(OK)
	}
	GetATR(baATR);
	ATR=baATR;

	_return(OK)
	exit_func
	_return(FAIL)
}

}
