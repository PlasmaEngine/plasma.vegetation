#include <EditorPluginVegetation/EditorPluginVegetationPCH.h>

#include <EditorPluginVegetation/VegetationTypeAsset/VegetationTypeAsset.h>
#include <EditorPluginVegetation/VegetationTypeAsset/VegetationTypeAssetManager.h>
#include <EditorPluginVegetation/VegetationTypeAsset/VegetationTypeAssetWindow.h>

// clang-format off
PL_BEGIN_DYNAMIC_REFLECTED_TYPE(plVegetationTypeAssetDocumentManager, 1, plRTTIDefaultAllocator<plVegetationTypeAssetDocumentManager>)
PL_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

plVegetationTypeAssetDocumentManager::plVegetationTypeAssetDocumentManager()
{
  plDocumentManager::s_Events.AddEventHandler(plMakeDelegate(&plVegetationTypeAssetDocumentManager::OnDocumentManagerEvent, this));

  m_DocTypeDesc.m_sDocumentTypeName = "Vegetation Type";
  m_DocTypeDesc.m_sFileExtension = "plVegetationTypeAsset";
  m_DocTypeDesc.m_sIcon = ":/AssetIcons/Vegetation_Type.svg";
  m_DocTypeDesc.m_sAssetCategory = "Rendering";
  m_DocTypeDesc.m_pDocumentType = plGetStaticRTTI<plVegetationTypeAssetDocument>();
  m_DocTypeDesc.m_pManager = this;
  m_DocTypeDesc.m_CompatibleTypes.PushBack("CompatibleAsset_Vegetation_Type");

  m_DocTypeDesc.m_sResourceFileExtension = "plBinVegetationType";
  m_DocTypeDesc.m_AssetDocumentFlags = plAssetDocumentFlags::SupportsThumbnail;
}

plVegetationTypeAssetDocumentManager::~plVegetationTypeAssetDocumentManager()
{
  plDocumentManager::s_Events.RemoveEventHandler(plMakeDelegate(&plVegetationTypeAssetDocumentManager::OnDocumentManagerEvent, this));
}

void plVegetationTypeAssetDocumentManager::OnDocumentManagerEvent(const plDocumentManager::Event& e)
{
  switch (e.m_Type)
  {
    case plDocumentManager::Event::Type::DocumentWindowRequested:
    {
      if (e.m_pDocument->GetDynamicRTTI() == plGetStaticRTTI<plVegetationTypeAssetDocument>())
      {
        new plVegetationTypeAssetDocumentWindow(static_cast<plVegetationTypeAssetDocument*>(e.m_pDocument)); // NOLINT: Not a memory leak
      }
    }
    break;

    default:
      break;
  }
}

void plVegetationTypeAssetDocumentManager::InternalCreateDocument(plStringView sDocumentTypeName, plStringView sPath, bool bCreateNewDocument, plDocument*& out_pDocument, const plDocumentObject* pOpenContext)
{
  PL_IGNORE_UNUSED(sDocumentTypeName);
  PL_IGNORE_UNUSED(bCreateNewDocument);
  PL_IGNORE_UNUSED(pOpenContext);

  out_pDocument = new plVegetationTypeAssetDocument(sPath);
}

void plVegetationTypeAssetDocumentManager::InternalGetSupportedDocumentTypes(plDynamicArray<const plDocumentTypeDescriptor*>& inout_DocumentTypes) const
{
  inout_DocumentTypes.PushBack(&m_DocTypeDesc);
}
