#include "../stdafx.h"
#include "mac.h"

static char *szCompiledFile=__FILE__;

#ifdef WIN32

class init_mac {
public:
	BCRYPT_ALG_HANDLE algo;
	init_mac() {
		if (BCryptOpenAlgorithmProvider(&algo, BCRYPT_3DES_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0) != 0)
			throw std::runtime_error("Errore nell'inizializzazione dell'algoritmo MAC");
	}
	~init_mac() {
		BCryptCloseAlgorithmProvider(algo, 0);
	}
} algo_mac;

void CMAC::Init(ByteArray &key)
{
	init_func
	size_t KeySize = key.size();
	ER_ASSERT((KeySize % 8) == 0, "Errore nella lunghezza della chiave MAC (non divisibile per 8)")

	ER_ASSERT(KeySize >= 8 && KeySize <= 32, "Errore nella lunghezza della chiave MAC (<8 o >32)")
	ByteDynArray BCrytpKey;
	iv.resize(8);
	iv.fill(0);
	switch (KeySize) {
	case 8:
		BCrytpKey.set(&key, &key, &key);
		break;
	case 16:
		BCrytpKey.set(&key, &key.left(8));
		break;
	case 24:
		BCrytpKey = key;
		break;
	case 32:
		BCrytpKey = key.left(24);
		iv = key.right(8);
		break;
	}

	ByteDynArray k1;
	k1.set(&key.left(8), &key.left(8), &key.left(8));

	if (BCryptGenerateSymmetricKey(algo_mac.algo, &this->key1, nullptr, 0, k1.data(), (ULONG)k1.size(), 0) != 0)
		throw std::runtime_error("Errore nella creazione della chiave MAC");

	if (BCryptGenerateSymmetricKey(algo_mac.algo, &this->key2, nullptr, 0, BCrytpKey.data(), (ULONG)BCrytpKey.size(), 0) != 0)
		throw std::runtime_error("Errore nella creazione della chiave MAC");

	exit_func
}

CMAC::CMAC() : key1(nullptr), key2(nullptr) {
}

CMAC::~CMAC(void)
{
	if (key1!=nullptr)
		BCryptDestroyKey(key1);
	if (key2 != nullptr)
		BCryptDestroyKey(key2);
}

DWORD CMAC::_Mac(const ByteArray &data,ByteDynArray &resp)
{
	init_func

	size_t ANSILen=ANSIPadLen(data.size());
	ByteDynArray iv = this->iv;

	if (data.size()>8) {
		ByteDynArray OutTmp(ANSILen - 8);
		ULONG result = (ULONG)OutTmp.size();
		if (BCryptEncrypt(key1, data.data(), (long)ANSILen - 8, nullptr, iv.data(), (ULONG)iv.size(), OutTmp.data(), (ULONG)OutTmp.size(), &result, 0) != 0)
			throw std::runtime_error("Errore nel calcolo del MAC");
	}

	resp.resize(8);
	ULONG result = (ULONG)resp.size();
	if (BCryptEncrypt(key2, data.mid(ANSILen - 8).data(), (long)(data.size() - ANSILen) + 8, nullptr, iv.data(), (ULONG)iv.size(), resp.data(), (ULONG)resp.size(), &result, 0) != 0)
		throw std::runtime_error("Errore nel calcolo del MAC");

	_return(OK)
	exit_func
	_return(FAIL)
}
#else

void CMAC::Init(ByteArray &key)
{
	init_func
	size_t KeySize = key.size();
	ER_ASSERT((KeySize % 8) == 0, "Errore nella lunghezza della chiave MAC (non divisibile per 8)")

		ER_ASSERT(KeySize >= 8 && KeySize <= 32, "Errore nella lunghezza della chiave MAC (<8 o >32)")
		des_cblock *keyVal1 = nullptr, *keyVal2 = nullptr, *keyVal3 = nullptr;

	switch (KeySize) {
	case 8:
		memset(initVec, 0, 8);
		keyVal1 = keyVal2 = keyVal3 = (des_cblock *)key.data();
		break;
	case 16:
		memset(initVec, 0, 8);
		keyVal1 = keyVal3 = (des_cblock *)key.data();
		keyVal2 = (des_cblock *)key.mid(8).data();
		break;
	case 24:
		memset(initVec, 0, 8);
		keyVal1 = (des_cblock *)key.data();
		keyVal2 = (des_cblock *)key.mid(8).data();
		keyVal3 = (des_cblock *)key.mid(16).data();
		break;
	case 32:
		memcpy_s(initVec, sizeof(des_cblock), key.mid(24).data(), 8);
		keyVal1 = (des_cblock *)key.data();
		keyVal2 = (des_cblock *)key.mid(8).data();
		keyVal3 = (des_cblock *)key.mid(16).data();
	}

	des_set_key(keyVal1, k1);
	des_set_key(keyVal2, k2);
	des_set_key(keyVal3, k3);

	exit_func
}

CMAC::~CMAC(void)
{
}
DWORD CMAC::_Mac(const ByteArray &data, ByteDynArray &resp)
{
	init_func

	des_cblock iv;
	memcpy_s(iv, sizeof(des_cblock), initVec, sizeof(initVec));

	size_t ANSILen = ANSIPadLen(data.size());
	if (data.size()>8) {
		ByteDynArray baOutTmp(ANSILen - 8);
		des_ncbc_encrypt(data.data(), baOutTmp.data(), (long)ANSILen - 8, k1, &iv, DES_ENCRYPT);
	}
	uint8_t dest[8];
	des_ede3_cbc_encrypt(data.mid(ANSILen - 8).data(), dest, (long)(data.size() - ANSILen) + 8, k1, k2, k3, &iv, DES_ENCRYPT);
	resp = ByteArray(dest, 8);

	_return(OK)
	exit_func
	_return(FAIL)
}

CMAC::CMAC() {
}
#endif


CMAC::CMAC(ByteArray &key) {
	Init(key);
}

ByteDynArray CMAC::Mac(const ByteArray &data)
{
	init_func
	ByteDynArray result;
	ER_CALL(_Mac(data,result),"Errore nel calcolo del MAC");
	_return (result)
	exit_func
}
