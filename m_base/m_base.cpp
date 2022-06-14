#include "m_base.hpp"

Base m_Base;
void Base::Destroy(Base* m_Element) {
	delete m_Element;
}
