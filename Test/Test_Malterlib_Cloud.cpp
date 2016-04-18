
#include <Mib/Core/Core>
#include <Mib/Test/Test>

class CCloud_Tests : public NMib::NTest::CTest
{
public:
	
	void f_DoTests()
	{
	}
};

DMibTestRegister(CCloud_Tests, Malterlib::Cloud);
