/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright 2005 - 2016 Zarafa and its licensors
 */
#pragma once
#include <atomic>
#include <mapidefs.h>

class M4LUnknown : public virtual IUnknown {
private:
	std::atomic<unsigned int> ref{0};
    
public:
	virtual ~M4LUnknown(void) = default;
	virtual ULONG AddRef() override;
	virtual ULONG Release() override;
	virtual HRESULT QueryInterface(const IID &, void **) override;
};
