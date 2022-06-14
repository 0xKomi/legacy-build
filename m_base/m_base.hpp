#pragma once
#include "../ui_elements/new_menu.h"

class Base {
	virtual void Destroy(Base* m_Element);
};
extern Base m_Base;