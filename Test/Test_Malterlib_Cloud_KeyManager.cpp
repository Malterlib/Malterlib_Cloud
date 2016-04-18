
#include <Mib/Core/Core>
#include <Mib/Test/Test>
#include <Mib/Cloud/KeyManager>

class CKeyManager_Tests : public NMib::NTest::CTest
{
public:
	
	void f_DoTests()
	{
	}
};

DMibTestRegister(CKeyManager_Tests, Malterlib::Cloud);
