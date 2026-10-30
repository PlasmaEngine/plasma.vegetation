#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorPluginVegetation/GrassTypeAsset/GrassTypeAsset.h>
#include <EditorPluginVegetation/GrassTypeAsset/GrassTypeAssetManager.h>
#include <EditorPluginVegetation/GrassTypeAsset/GrassTypeAssetWindow.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plGrassTypeAssetDocumentManager, 1, plRTTIDefaultAllocator<plGrassTypeAssetDocumentManager>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plGrassTypeAssetDocumentManager::plGrassTypeAssetDocumentManager()
{
  plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plGrassTypeAssetDocumentManager::OnDocumentManagerEvent, this));

  m_DocTypeDesc.m_sDocumentTypeName = "Grass Type";
  m_DocTypeDesc.m_sFileExtension = "plGrassTypeAsset";
  m_DocTypeDesc.m_sIcon = ":/AssetIcons/Grass_Type.svg";
  m_DocTypeDesc.m_sAssetCategory = "Rendering";
  m_DocTypeDesc.m_pDocumentType = plGetStaticRTTI<plGrassTypeAssetDocument>();
  m_DocTypeDesc.m_pManager = this;
  m_DocTypeDesc.m_CompatibleTypes.PushBack("CompatibleAsset_Grass_Type");

  m_DocTypeDesc.m_sResourceFileExtension = "plBinGrassType";
  m_DocTypeDesc.m_AssetDocumentFlags = plAssetDocumentFlags::SupportsThumbnail;
}

plGrassTypeAssetDocumentManager::~plGrassTypeAssetDocumentManager()
{
  plDocumentManager::s_Events.RemoveEventHandler(plMakeDelegate(&plGrassTypeAssetDocumentManager::OnDocumentManagerEvent, this));
}

void plGrassTypeAssetDocumentManager::OnDocumentManagerEvent(const plDocumentManager::Event& e)
{
  switch (e.m_Type)
  {
    case plDocumentManager::Event::Type::DocumentWindowRequested:
    {
      if (e.m_pDocument->GetDynamicRTTI() == plGetStaticRTTI<plGrassTypeAssetDocument>())
      {
        new plGrassTypeAssetDocumentWindow(static_cast<plGrassTypeAssetDocument*>(e.m_pDocument)); // NOLINT: Not a memory leak
      }
    }
    break;

    default:
      break;
  }
}

void plGrassTypeAssetDocumentManager::InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext)
{
  PL_IGNORE_UNUSED(sDocumentTypeName);
  PL_IGNORE_UNUSED(bCreateNewDocument);
  PL_IGNORE_UNUSED(pOpenContext);

  out_pDocument = new plGrassTypeAssetDocument(sPath);
}

void plGrassTypeAssetDocumentManager::InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const
{
  inout_DocumentTypes.PushBack(&m_DocTypeDesc);
}
