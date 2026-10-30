#pragma once

#include <EditorFramework/Assets/AssetDocumentManager.h>
#include <EditorPluginVegetation/EditorPluginVegetationDLL.h>
#include <Foundation/Types/Status.h>

class plGrassTypeAssetDocumentManager : public plAssetDocumentManager
{
  PL_ADD_DYNAMIC_REFLECTION(plGrassTypeAssetDocumentManager, plAssetDocumentManager);

public:
  plGrassTypeAssetDocumentManager();
  ~plGrassTypeAssetDocumentManager();

private:
  void OnDocumentManagerEvent(const plDocumentManager::Event& e);

  virtual void InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext) override;
  virtual void InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const override;

  virtual bool GeneratesProfileSpecificAssets() const override { return false; }

  plAssetDocumentTypeDescriptor m_DocTypeDesc;
};
