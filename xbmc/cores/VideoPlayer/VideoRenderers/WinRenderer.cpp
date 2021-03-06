/*
 *      Copyright (C) 2005-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include <ppltasks.h>

#include "WinRenderer.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"
#include "cores/FFmpeg.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "guilib/LocalizeStrings.h"
#include "platform/win32/WIN32Util.h"
#include "RenderFactory.h"
#include "RenderFlags.h"
#include "settings/AdvancedSettings.h"
#include "settings/DisplaySettings.h"
#include "settings/MediaSettings.h"
#include "settings/Settings.h"
#include "ServiceBroker.h"
#include "threads/SingleLock.h"
#include "utils/log.h"
#include "utils/win32/gpu_memcpy_sse4.h"
#include "VideoShaders/WinVideoFilter.h"
#include "windowing/WindowingFactory.h"

typedef struct 
{
  RenderMethod  method;
  const char   *name;
} RenderMethodDetail;

static RenderMethodDetail RenderMethodDetails[] = 
{
    { RENDER_SW     , "Software" },
    { RENDER_PS     , "Pixel Shaders" },
    { RENDER_DXVA   , "DXVA" },
    { RENDER_INVALID, nullptr }
};

static RenderMethodDetail *FindRenderMethod(RenderMethod m)
{
  for (unsigned i = 0; RenderMethodDetails[i].method != RENDER_INVALID; i++) {
    if (RenderMethodDetails[i].method == m)
      return &RenderMethodDetails[i];
  }
  return nullptr;
}

CBaseRenderer* CWinRenderer::Create(CVideoBuffer *buffer)
{
  return new CWinRenderer();
}

bool CWinRenderer::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("default", CWinRenderer::Create);
  return true;
}

CWinRenderer::CWinRenderer() : CBaseRenderer()
  , m_bConfigured(false)
  , m_bUseHQScaler(false)
  , m_bFilterInitialized(false)
  , m_cmsOn(false)
  , m_clutLoaded(true)
  , m_destWidth(0)
  , m_destHeight(0)
  , m_frameIdx(0)
  , m_iYV12RenderBuffer(0)
  , m_NumYV12Buffers(0)
  , m_neededBuffers(0)
  , m_iRequestedMethod(RENDER_METHOD_AUTO)
  , m_dxva_format(DXGI_FORMAT_UNKNOWN)
  , m_renderMethod(RENDER_PS)
  , m_bufferFormat(BUFFER_FMT_NONE)
  , m_scalingMethod(VS_SCALINGMETHOD_LINEAR)
  , m_scalingMethodGui(static_cast<ESCALINGMETHOD>(-1))
  , m_processor(nullptr)
  , m_sw_scale_ctx(nullptr)
  , m_colorShader(nullptr)
  , m_scalerShader(nullptr)
  , m_capture(nullptr)
  , m_pCLUTView(nullptr)
{
  m_format = AV_PIX_FMT_NONE;
  for (unsigned i = 0; i < NUM_BUFFERS; i++)
    m_renderBuffers[i].Release();

  m_colorManager = std::make_unique<CColorManager>();
  m_outputShader.reset();
  m_useDithering = CServiceBroker::GetSettings().GetBool("videoscreen.dither");
  m_ditherDepth = CServiceBroker::GetSettings().GetInt("videoscreen.ditherdepth");

  PreInit();
}

CWinRenderer::~CWinRenderer()
{
  CWinRenderer::UnInit();
}

void CWinRenderer::ManageTextures()
{
  if( m_NumYV12Buffers < m_neededBuffers )
  {
    for(int i = m_NumYV12Buffers; i<m_neededBuffers;i++)
      CreateRenderBuffer(i);

    m_NumYV12Buffers = m_neededBuffers;
  }
  else if( m_NumYV12Buffers > m_neededBuffers )
  {
    for (int i = m_NumYV12Buffers - 1; i >= m_neededBuffers; i--)
      DeleteRenderBuffer(i);

    m_NumYV12Buffers = m_neededBuffers;
    m_iYV12RenderBuffer = m_iYV12RenderBuffer % m_NumYV12Buffers;
  }
}

void CWinRenderer::SelectRenderMethod()
{
  EBufferFormat dxFormat = SelectBufferFormat(m_format, RENDER_DXVA);
  EBufferFormat psFormat = SelectBufferFormat(m_format, RENDER_PS);
  // modern drivers allow using HW pic in shaders
  bool allowChangeMethod = dxFormat == psFormat && m_iRequestedMethod != RENDER_SW;

  // old drivers + HW decoded picture -> we must force DXVA render method
  if (!allowChangeMethod && m_format == AV_PIX_FMT_D3D11VA_VLD)
  {
    CLog::Log(LOGNOTICE, "%s: rendering method forced to DXVA processor.", __FUNCTION__);
    m_renderMethod = RENDER_DXVA;
    if (!m_processor || !m_processor->Open(m_sourceWidth, m_sourceHeight))
    {
      CLog::Log(LOGNOTICE, "%s: unable to open DXVA processor.", __FUNCTION__);
      if (m_processor)
        m_processor->Close();
      m_renderMethod = RENDER_INVALID;
    }
  }
  else
  {
    CLog::Log(LOGDEBUG, "%s: requested render method: %d", __FUNCTION__, m_iRequestedMethod);
    switch (m_iRequestedMethod)
    {
    case RENDER_METHOD_DXVA:
    {
      // skip if format isn't DXVA compatible.
      if (dxFormat >= BUFFER_FMT_D3D11_BYPASS)
      {
        if (m_processor && m_processor->Open(m_sourceWidth, m_sourceHeight))
        {
          m_renderMethod = RENDER_DXVA;
          break;
        }
        allowChangeMethod = false;
        CLog::Log(LOGNOTICE, "%s: unable to open DXVA processor.", __FUNCTION__);
        if (m_processor)
          m_processor->Close();
      }
    }
    // fallback to auto
    case RENDER_METHOD_AUTO:
      if (allowChangeMethod)
      {
        // for modern drivers select method depends on input
        // for HW decoded or interlaced picture prefer DXVA method.
        if (m_format == AV_PIX_FMT_D3D11VA_VLD || m_iFlags & DVP_FLAG_INTERLACED)
        {
          if (m_processor && m_processor->Open(m_sourceWidth, m_sourceHeight))
          {
            m_renderMethod = RENDER_DXVA;
            break;
          }
          CLog::Log(LOGNOTICE, "%s: unable to open DXVA processor", __FUNCTION__);
          if (m_processor)
            m_processor->Close();
        }
      }
    // drop through to pixel shader
    case RENDER_METHOD_D3D_PS:
    {
      CTestShader shader;
      if (shader.Create())
      {
        m_renderMethod = RENDER_PS;
        break;
      }
      // this is something out of the ordinary
      CLog::Log(LOGNOTICE, "%s: unable to load test shader - D3D installation is most likely incomplete, falling back to SW mode.", __FUNCTION__);
      CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Warning, "DirectX", g_localizeStrings.Get(2101));
    }
    // drop through to software
    case RENDER_METHOD_SOFTWARE:
    default:
      // so we'll do the color conversion in software.
      m_renderMethod = RENDER_SW;
      break;
    }
  }

  m_bufferFormat = SelectBufferFormat(m_format, m_renderMethod);
  m_frameIdx = 0;

  RenderMethodDetail *rmdet = FindRenderMethod(m_renderMethod);
  CLog::Log(LOGDEBUG, "%s: selected render method %d: %s", __FUNCTION__, m_renderMethod, rmdet != nullptr ? rmdet->name : "unknown");
  CLog::Log(LOGDEBUG, "%s: selected buffer format %d", __FUNCTION__, m_bufferFormat);
}

bool CWinRenderer::Configure(const VideoPicture &picture, float fps, unsigned flags, unsigned int orientation)
{
  m_sourceWidth       = picture.iWidth;
  m_sourceHeight      = picture.iHeight;
  m_renderOrientation = orientation;
  // need to recreate textures
  m_NumYV12Buffers    = 0;
  m_iYV12RenderBuffer = 0;
  // reinitialize the filters/shaders
  m_bFilterInitialized = false;

  m_fps = fps;
  m_iFlags = flags;
  m_format = picture.videoBuffer->GetFormat();
  if (m_format == AV_PIX_FMT_D3D11VA_VLD)
  {
    DXVA::CDXVAOutputBuffer *dxvaBuf = dynamic_cast<DXVA::CDXVAOutputBuffer*>(picture.videoBuffer);
    m_dxva_format = dxvaBuf->format;
  }

  // calculate the input frame aspect ratio
  CalculateFrameAspectRatio(picture.iDisplayWidth, picture.iDisplayHeight);
  SetViewMode(CMediaSettings::GetInstance().GetCurrentVideoSettings().m_ViewMode);
  ManageRenderArea();

  SelectRenderMethod();
  m_bConfigured = true;

  // load 3DLUT
  ColorManagmentUpdate();

  return true;
}

int CWinRenderer::NextBuffer() const
{
  if(m_NumYV12Buffers)
    return (m_iYV12RenderBuffer + 1) % m_NumYV12Buffers;
  return -1;
}

void CWinRenderer::AddVideoPicture(const VideoPicture &picture, int index, double currentClock)
{
  unsigned flags = RenderManager::GetFlagsChromaPosition(picture.chroma_position)
    | RenderManager::GetFlagsColorMatrix(picture.color_matrix, picture.iWidth, picture.iHeight)
    | RenderManager::GetFlagsColorPrimaries(picture.color_primaries)
    | RenderManager::GetFlagsColorTransfer(picture.color_transfer);
  if (picture.color_range == 1)
    flags |= CONF_FLAGS_YUV_FULLRANGE;

  m_renderBuffers[index].videoBuffer = picture.videoBuffer;
  m_renderBuffers[index].videoBuffer->Acquire();
  m_renderBuffers[index].frameIdx = m_frameIdx;
  m_renderBuffers[index].flags = flags;
  m_frameIdx += 2;

  if (picture.videoBuffer->GetFormat() == AV_PIX_FMT_D3D11VA_VLD)
    m_renderBuffers[index].QueueCopyBuffer();
  m_renderBuffers[index].loaded = false;
}

void CWinRenderer::Reset()
{
}

void CWinRenderer::Update()
{
  if (!m_bConfigured) 
    return;
  ManageRenderArea();
  ManageTextures();
}

void CWinRenderer::RenderUpdate(bool clear, unsigned int flags, unsigned int alpha)
{
  if (clear)
    g_graphicsContext.Clear(g_Windowing.UseLimitedColor() ? 0x101010 : 0);

  if (!m_bConfigured)
    return;

  g_Windowing.SetAlphaBlendEnable(alpha < 255);
  ManageTextures();
  ManageRenderArea();
  Render(flags, g_Windowing.GetBackBuffer());
}

void CWinRenderer::FlipPage(int source)
{
  if( source >= 0 && source < m_NumYV12Buffers )
    m_iYV12RenderBuffer = source;
  else
    m_iYV12RenderBuffer = NextBuffer();
}

void CWinRenderer::PreInit()
{
  CSingleLock lock(g_graphicsContext);
  m_bConfigured = false;
  UnInit();

  m_iRequestedMethod = CServiceBroker::GetSettings().GetInt(CSettings::SETTING_VIDEOPLAYER_RENDERMETHOD);

  m_processor = new DXVA::CProcessorHD();
  if (!m_processor->PreInit())
  {
    CLog::Log(LOGNOTICE, "%: - could not init DXVA processor - skipping.", __FUNCTION__);
    SAFE_DELETE(m_processor);
  }
}

void CWinRenderer::UnInit()
{
  CSingleLock lock(g_graphicsContext);

  if (m_IntermediateTarget.Get())
    m_IntermediateTarget.Release();

  SAFE_DELETE(m_colorShader);
  SAFE_DELETE(m_scalerShader);
  
  m_bConfigured = false;
  m_bFilterInitialized = false;

  for(int i = 0; i < NUM_BUFFERS; i++)
    DeleteRenderBuffer(i);

  m_NumYV12Buffers = 0;

  if (m_sw_scale_ctx)
  {
    sws_freeContext(m_sw_scale_ctx);
    m_sw_scale_ctx = nullptr;
  }

  if (m_processor)
  {
    m_processor->UnInit();
    SAFE_DELETE(m_processor);
  }
  SAFE_RELEASE(m_pCLUTView);
  m_outputShader.reset();
}

void CWinRenderer::Flush()
{
  if (!m_bConfigured)
    return;

  for (int i = 0; i < NUM_BUFFERS; i++)
    DeleteRenderBuffer(i);

  m_iYV12RenderBuffer = 0;
  m_NumYV12Buffers = 0;
  m_bFilterInitialized = false;
}

bool CWinRenderer::CreateIntermediateRenderTarget(unsigned int width, unsigned int height, bool dynamic)
{
  unsigned int usage = D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;

  DXGI_FORMAT format = DXGI_FORMAT_B8G8R8X8_UNORM;
  if      (m_renderMethod == RENDER_DXVA)                                   format = DXGI_FORMAT_B8G8R8X8_UNORM;
  else if (g_Windowing.IsFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM, usage))  format = DXGI_FORMAT_B8G8R8A8_UNORM;
  else if (g_Windowing.IsFormatSupport(DXGI_FORMAT_B8G8R8X8_UNORM, usage))  format = DXGI_FORMAT_B8G8R8X8_UNORM;

  // don't create new one if it exists with requested size and format
  if ( m_IntermediateTarget.Get() && m_IntermediateTarget.GetFormat() == format
    && m_IntermediateTarget.GetWidth() == width && m_IntermediateTarget.GetHeight() == height)
    return true;

  if (m_IntermediateTarget.Get())
    m_IntermediateTarget.Release();

  CLog::Log(LOGDEBUG, "%s: format %i.", __FUNCTION__, format);

  if (!m_IntermediateTarget.Create(width, height, 1, dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT, format))
  {
    CLog::Log(LOGERROR, "%s: intermediate render target creation failed.", __FUNCTION__);
    return false;
  }
  return true;
}

template<typename T>
bool Contains(std::vector<T> vector, T item)
{
  return find(vector.begin(), vector.end(), item) != vector.end();
}

EBufferFormat CWinRenderer::SelectBufferFormat(AVPixelFormat format, const RenderMethod method) const
{
  AVPixelFormat decoderFormat = format;
  if (format == AV_PIX_FMT_D3D11VA_VLD)
  {
    switch(m_dxva_format)
    {
    case DXGI_FORMAT_NV12:
      decoderFormat = AV_PIX_FMT_NV12;
      break;
    case DXGI_FORMAT_P010:
      decoderFormat = AV_PIX_FMT_P010;
      break;
    case DXGI_FORMAT_P016:
      decoderFormat = AV_PIX_FMT_P016;
      break;
    default:
      break;
    }
  }

  // check shared formats and processor formats
  if ( method != RENDER_SW && Contains(g_Windowing.m_sharedFormats, decoderFormat)
    || (method == RENDER_DXVA && Contains(g_Windowing.m_processorFormats, decoderFormat)) )
  {
    switch (format)
    {
    case AV_PIX_FMT_D3D11VA_VLD:
      return BUFFER_FMT_D3D11_BYPASS;
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_YUV420P:
      return BUFFER_FMT_D3D11_NV12;
    case AV_PIX_FMT_YUV420P10:
    case AV_PIX_FMT_P010:
      return BUFFER_FMT_D3D11_P010;
    case AV_PIX_FMT_YUV420P16:
    case AV_PIX_FMT_P016:
      return BUFFER_FMT_D3D11_P016;
    default:
      return BUFFER_FMT_NONE;
    }
  }

  // fit decoder format for shaders
  if (format == AV_PIX_FMT_D3D11VA_VLD)
  {
    switch (m_dxva_format)
    {
    case DXGI_FORMAT_P010:
      decoderFormat = AV_PIX_FMT_YUV420P10;
      break;
    case DXGI_FORMAT_P016:
      decoderFormat = AV_PIX_FMT_YUV420P16;
      break;
    default: 
      break;
    }
  }

  // check common formats (SW rendering or win7)
  if ( method == RENDER_SW || (method == RENDER_PS && Contains(g_Windowing.m_shaderFormats, decoderFormat)))
  {
    switch (format)
    {
    case AV_PIX_FMT_D3D11VA_VLD:
    {
      switch(m_dxva_format)
      {
        // @todo implement support of these formats
        //case DXGI_FORMAT_P010:
        //  return BUFFER_FMT_YUV420P10;
        //case DXGI_FORMAT_P016:
        //  return BUFFER_FMT_YUV420P16;
        case DXGI_FORMAT_NV12:
        default:
          return BUFFER_FMT_NV12;
      }
    }
    case AV_PIX_FMT_NV12:
      return BUFFER_FMT_NV12;
    case AV_PIX_FMT_YUV420P:
      return BUFFER_FMT_YUV420P;
    case AV_PIX_FMT_YUV420P10:
      return BUFFER_FMT_YUV420P10;
    case AV_PIX_FMT_YUV420P16:
      return BUFFER_FMT_YUV420P16;
    // is they still used?
    case AV_PIX_FMT_YUYV422:
      return BUFFER_FMT_YUV420P16;
    case AV_PIX_FMT_UYVY422:
      return BUFFER_FMT_UYVY422;
    default:
      return BUFFER_FMT_NONE;
    }
  }

  return BUFFER_FMT_NONE;
}

void CWinRenderer::SelectSWVideoFilter()
{
  CreateIntermediateRenderTarget(m_sourceWidth, m_sourceHeight, true);
}

void CWinRenderer::SelectPSVideoFilter()
{
  m_bUseHQScaler = false;

  switch (m_scalingMethod)
  {
  case VS_SCALINGMETHOD_NEAREST:
  case VS_SCALINGMETHOD_LINEAR:
    break;

  case VS_SCALINGMETHOD_CUBIC:
  case VS_SCALINGMETHOD_LANCZOS2:
  case VS_SCALINGMETHOD_SPLINE36_FAST:
  case VS_SCALINGMETHOD_LANCZOS3_FAST:
    m_bUseHQScaler = true;
    break;

  case VS_SCALINGMETHOD_SPLINE36:
  case VS_SCALINGMETHOD_LANCZOS3:
    m_bUseHQScaler = true;
    break;

  case VS_SCALINGMETHOD_SINC8:
  case VS_SCALINGMETHOD_NEDI:
    CLog::Log(LOGERROR, "D3D: TODO: This scaler has not yet been implemented");
    break;

  case VS_SCALINGMETHOD_BICUBIC_SOFTWARE:
  case VS_SCALINGMETHOD_LANCZOS_SOFTWARE:
  case VS_SCALINGMETHOD_SINC_SOFTWARE:
    CLog::Log(LOGERROR, "D3D: TODO: Software scaling has not yet been implemented");
    break;

  default:
    break;
  }

  if (m_scalingMethod == VS_SCALINGMETHOD_AUTO)
  {
    bool scaleSD = m_sourceHeight < 720 && m_sourceWidth < 1280;
    bool scaleUp = static_cast<int>(m_sourceHeight) < g_graphicsContext.GetHeight() 
                && static_cast<int>(m_sourceWidth) < g_graphicsContext.GetWidth();
    bool scaleFps = m_fps < (g_advancedSettings.m_videoAutoScaleMaxFps + 0.01f);

    if (m_renderMethod == RENDER_DXVA)
    {
      m_scalingMethod = VS_SCALINGMETHOD_DXVA_HARDWARE;
      m_bUseHQScaler = false;
    }
    else if (scaleSD && scaleUp && scaleFps && Supports(VS_SCALINGMETHOD_LANCZOS3_FAST))
    {
      m_scalingMethod = VS_SCALINGMETHOD_LANCZOS3_FAST;
      m_bUseHQScaler = true;
    }
  }
  if (m_renderOrientation)
    m_bUseHQScaler = false;
}

void CWinRenderer::UpdatePSVideoFilter()
{
  SAFE_DELETE(m_scalerShader);

  if (m_bUseHQScaler)
  {
    // First try the more efficient two pass convolution scaler
    m_scalerShader = new CConvolutionShaderSeparable();

    if (!m_scalerShader->Create(m_scalingMethod, m_outputShader.get()))
    {
      SAFE_DELETE(m_scalerShader);
      CLog::Log(LOGNOTICE, "%s: two pass convolution shader init problem, falling back to one pass.", __FUNCTION__);
    }

    // Fallback on the one pass version
    if (m_scalerShader == nullptr)
    {
      m_scalerShader = new CConvolutionShader1Pass();

      if (!m_scalerShader->Create(m_scalingMethod, m_outputShader.get()))
      {
        SAFE_DELETE(m_scalerShader);
        CGUIDialogKaiToast::QueueNotification(CGUIDialogKaiToast::Error, g_localizeStrings.Get(34400), g_localizeStrings.Get(34401));
        m_bUseHQScaler = false;
      }
    }
  }

  if (m_bUseHQScaler && !CreateIntermediateRenderTarget(m_sourceWidth, m_sourceHeight, false))
  {
    SAFE_DELETE(m_scalerShader);
    m_bUseHQScaler = false;
  }

  SAFE_DELETE(m_colorShader);

  if (m_renderMethod == RENDER_DXVA)
  {
    // we'd use m_IntermediateTarget as rendering target for possible anaglyph stereo with dxva processor.
    if (!m_bUseHQScaler)
      CreateIntermediateRenderTarget(m_destWidth, m_destHeight, false);
    // When using DXVA, we are already setup at this point, color shader is not needed
    return;
  }

  m_colorShader = new CYUV2RGBShader();
  if (!m_colorShader->Create(m_bufferFormat, m_bUseHQScaler ? nullptr : m_outputShader.get()))
  {
    if (m_bUseHQScaler)
    {
      m_IntermediateTarget.Release();
      SAFE_DELETE(m_scalerShader);
    }
    SAFE_DELETE(m_colorShader);
    m_bUseHQScaler = false;

    // we're in big trouble - fallback to sw method
    m_renderMethod = RENDER_SW;
    if (m_NumYV12Buffers)
    {
      m_NumYV12Buffers = 0;
      ManageTextures();
    }
    SelectSWVideoFilter();
  }
}

void CWinRenderer::UpdateVideoFilter()
{
  bool cmsChanged = m_cmsOn != m_colorManager->IsEnabled()
                 || m_cmsOn && !m_colorManager->CheckConfiguration(m_cmsToken, m_iFlags);
  cmsChanged &= m_clutLoaded;

  if (m_scalingMethodGui == CMediaSettings::GetInstance().GetCurrentVideoSettings().m_ScalingMethod
   && m_bFilterInitialized && !cmsChanged)
    return;

  m_bFilterInitialized = true;
  m_scalingMethodGui = CMediaSettings::GetInstance().GetCurrentVideoSettings().m_ScalingMethod;
  m_scalingMethod    = m_scalingMethodGui;

  if (!Supports(m_scalingMethod))
  {
    CLog::Log(LOGWARNING, "%s: chosen scaling method %d is not supported by renderer", __FUNCTION__, static_cast<int>(m_scalingMethod));
    m_scalingMethod = VS_SCALINGMETHOD_AUTO;
  }

  if (cmsChanged)
    ColorManagmentUpdate();

  if (cmsChanged || !m_outputShader)
  {
    m_outputShader = std::make_unique<COutputShader>();
    if (!m_outputShader->Create(m_cmsOn, m_useDithering, m_ditherDepth))
    {
      CLog::Log(LOGDEBUG, "%s: Unable to create output shader.", __FUNCTION__);
      m_outputShader.reset();
    }
    else if (m_pCLUTView && m_CLUTSize)
      m_outputShader->SetCLUT(m_CLUTSize, m_pCLUTView);
  }

  RESOLUTION_INFO res = g_graphicsContext.GetResInfo();
  if (!res.bFullScreen)
    res = g_graphicsContext.GetResInfo(RES_DESKTOP);

  m_destWidth = res.iScreenWidth;
  m_destHeight = res.iScreenHeight;

  switch(m_renderMethod)
  {
  case RENDER_SW:
    SelectSWVideoFilter();
    break;

  case RENDER_PS:
  case RENDER_DXVA:
    SelectPSVideoFilter();
    UpdatePSVideoFilter();
    break;

  default:
    return;
  }
}

void CWinRenderer::Render(DWORD flags, CD3DTexture* target)
{
  if (!m_renderBuffers[m_iYV12RenderBuffer].loaded)
  {
    if (!m_renderBuffers[m_iYV12RenderBuffer].UploadBuffer())
      return;
  }

  UpdateVideoFilter();

  switch (m_renderMethod)
  {
  case RENDER_DXVA:
    RenderHW(flags, target);
    break;
  case RENDER_PS:
    RenderPS(target);
    break;
  case RENDER_SW:
    RenderSW(target);
    break;
  default:
    return;
  }

  if (m_bUseHQScaler)
    RenderHQ(target);

  g_Windowing.ApplyStateBlock();
}

void CWinRenderer::RenderSW(CD3DTexture* target)
{
  // if creation failed
  if (!m_outputShader)
    return;

  // Don't know where this martian comes from but it can happen in the initial frames of a video
  if (m_destRect.x1 < 0 && m_destRect.x2 < 0 
   || m_destRect.y1 < 0 && m_destRect.y2 < 0)
    return;

  // fit format in case of hw decoder
  AVPixelFormat decoderFormat = m_format;
  if (m_format == AV_PIX_FMT_D3D11VA_VLD)
  {
    switch (m_dxva_format)
    {
    case DXGI_FORMAT_NV12:
      decoderFormat = AV_PIX_FMT_NV12;
      break;
    case DXGI_FORMAT_P010:
      decoderFormat = AV_PIX_FMT_YUV420P10;
      break;
    case DXGI_FORMAT_P016:
      decoderFormat = AV_PIX_FMT_YUV420P16;
      break;
    default:
      break;
    }
  }

  // 1. convert yuv to rgb
  m_sw_scale_ctx = sws_getCachedContext(m_sw_scale_ctx,
                                        m_sourceWidth, m_sourceHeight, decoderFormat,
                                        m_sourceWidth, m_sourceHeight, AV_PIX_FMT_BGRA,
                                        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

  CRenderBuffer* buf = &m_renderBuffers[m_iYV12RenderBuffer];

  uint8_t* src[YuvImage::MAX_PLANES];
  int srcStride[YuvImage::MAX_PLANES];

  for (unsigned int idx = 0; idx < buf->GetActivePlanes(); idx++)
    buf->MapPlane(idx, reinterpret_cast<void**>(&src[idx]), &srcStride[idx]);
  
  D3D11_MAPPED_SUBRESOURCE destlr;
  if (!m_IntermediateTarget.LockRect(0, &destlr, D3D11_MAP_WRITE_DISCARD))
    CLog::Log(LOGERROR, "%s: failed to lock swtarget texture into memory.", __FUNCTION__);

  uint8_t *dst[] = { static_cast<uint8_t*>(destlr.pData), nullptr, nullptr };
  int dstStride[] = { static_cast<int>(destlr.RowPitch), 0, 0 };

  sws_scale(m_sw_scale_ctx, src, srcStride, 0, m_sourceHeight, dst, dstStride);

  for (unsigned int idx = 0; idx < buf->GetActivePlanes(); idx++)
    buf->UnmapPlane(idx);

  if (!m_IntermediateTarget.UnlockRect(0))
    CLog::Log(LOGERROR, "%s: failed to unlock swtarget texture.", __FUNCTION__);

  // 2. output to display

  CVideoSettings settings = CMediaSettings::GetInstance().GetCurrentVideoSettings();
  m_outputShader->Render(m_IntermediateTarget, m_sourceWidth, m_sourceHeight, m_sourceRect, m_rotatedDestCoords, target,
                         g_Windowing.UseLimitedColor(), settings.m_Contrast * 0.01f, settings.m_Brightness * 0.01f);
}

void CWinRenderer::RenderPS(CD3DTexture* target)
{
  if (m_bUseHQScaler)
    target = &m_IntermediateTarget;

  CD3D11_VIEWPORT viewPort(0.0f, 0.0f, static_cast<float>(target->GetWidth()), static_cast<float>(target->GetHeight()));

  if (m_bUseHQScaler)
    g_Windowing.ResetScissors();

  // reset view port
  g_Windowing.Get3D11Context()->RSSetViewports(1, &viewPort);

  // select destination rectangle 
  CPoint destPoints[4];
  if (m_renderOrientation)
  {
    for (size_t i = 0; i < 4; i++)
      destPoints[i] = m_rotatedDestCoords[i];
  }
  else
  {
    CRect destRect = m_bUseHQScaler ? m_sourceRect : g_graphicsContext.StereoCorrection(m_destRect);
    destPoints[0] = { destRect.x1, destRect.y1 };
    destPoints[1] = { destRect.x2, destRect.y1 };
    destPoints[2] = { destRect.x2, destRect.y2 };
    destPoints[3] = { destRect.x1, destRect.y2 };
  }

  // render video frame
  m_colorShader->Render(m_sourceRect, destPoints,
                        CMediaSettings::GetInstance().GetCurrentVideoSettings().m_Contrast,
                        CMediaSettings::GetInstance().GetCurrentVideoSettings().m_Brightness,
                        &m_renderBuffers[m_iYV12RenderBuffer], target);
  // Restore our view port.
  g_Windowing.RestoreViewPort();
}

void CWinRenderer::RenderHQ(CD3DTexture* target)
{
  m_scalerShader->Render(m_IntermediateTarget, m_sourceWidth, m_sourceHeight, m_destWidth, m_destHeight
                       , m_sourceRect, g_graphicsContext.StereoCorrection(m_destRect)
                       , false, target);
}

void CWinRenderer::RenderHW(DWORD flags, CD3DTexture* target)
{
  CRenderBuffer *image = &m_renderBuffers[m_iYV12RenderBuffer];
  if ( image->format != BUFFER_FMT_D3D11_BYPASS
    && image->format != BUFFER_FMT_D3D11_NV12
    && image->format != BUFFER_FMT_D3D11_P010
    && image->format != BUFFER_FMT_D3D11_P016)
    return;
  
  if (!image->HasPic())
    return;

  int past = 0;
  int future = 0;

  CRenderBuffer* views[8];
  memset(views, 0, 8 * sizeof(CRenderBuffer*));
  views[2] = image;

  // set future frames
  while (future < 2)
  {
    bool found = false;
    for (int i = 0; i < m_NumYV12Buffers; i++)
    {
      if (m_renderBuffers[i].HasPic()
        && m_renderBuffers[i].frameIdx == image->frameIdx + (future*2 + 2))
      {
        views[1 - future++] = &m_renderBuffers[i];
        found = true;
        break;
      }
    }
    if (!found)
      break;
  }

  // set past frames
  while (past < 4)
  {
    bool found = false;
    for (int i = 0; i < m_NumYV12Buffers; i++)
    {
      if (m_renderBuffers[i].HasPic()
        && m_renderBuffers[i].frameIdx == image->frameIdx - (past*2 + 2))
      {
        views[3 + past++] = &m_renderBuffers[i];
        found = true;
        break;
      }
    }
    if (!found)
      break;
  }

  CRect destRect;
  switch (m_renderOrientation)
  {
  case 90:
    destRect = CRect(m_rotatedDestCoords[3], m_rotatedDestCoords[1]);
    break;
  case 180:
    destRect = m_destRect;
    break;
  case 270:
    destRect = CRect(m_rotatedDestCoords[1], m_rotatedDestCoords[3]);
    break;
  default:
    destRect = m_bUseHQScaler ? m_sourceRect : g_graphicsContext.StereoCorrection(m_destRect);
    break;
  }

  CRect src = m_sourceRect, dst = destRect;
  CRect targetRect = CRect(0.0f, 0.0f,
                       static_cast<float>(m_IntermediateTarget.GetWidth()), 
                       static_cast<float>(m_IntermediateTarget.GetHeight()));

  if (target != g_Windowing.GetBackBuffer())
  {
    // rendering capture
    targetRect.x2 = target->GetWidth();
    targetRect.y2 = target->GetHeight();
  }
  CWIN32Util::CropSource(src, dst, targetRect, m_renderOrientation);

  m_processor->Render(src, dst, m_IntermediateTarget.Get(), views, flags, image->frameIdx, m_renderOrientation);

  if (!m_bUseHQScaler)
  {
    if ( g_graphicsContext.GetStereoMode() == RENDER_STEREO_MODE_SPLIT_HORIZONTAL
      || g_graphicsContext.GetStereoMode() == RENDER_STEREO_MODE_SPLIT_VERTICAL)
    {
      CD3DTexture *backBuffer = g_Windowing.GetBackBuffer();
      CD3D11_VIEWPORT bbSize(0.f, 0.f, static_cast<float>(backBuffer->GetWidth()), static_cast<float>(backBuffer->GetHeight()));
      g_Windowing.Get3D11Context()->RSSetViewports(1, &bbSize);
    }

    // render frame
    m_outputShader->Render(m_IntermediateTarget, m_destWidth, m_destHeight, dst, dst, target);
    g_Windowing.RestoreViewPort();
  }
}

bool CWinRenderer::RenderCapture(CRenderCapture* capture)
{
  if (!m_bConfigured || m_NumYV12Buffers == 0)
    return false;

  bool succeeded = false;

  CRect saveSize = m_destRect;
  saveRotatedCoords();//backup current m_rotatedDestCoords

  m_destRect.SetRect(0, 0, static_cast<float>(capture->GetWidth()), static_cast<float>(capture->GetHeight()));
  syncDestRectToRotatedPoints();//syncs the changed destRect to m_rotatedDestCoords

  capture->BeginRender();
  if (capture->GetState() != CAPTURESTATE_FAILED)
  {
    Render(0, capture->GetTarget());
    capture->EndRender();
    succeeded = true;
  }

  m_destRect = saveSize;
  restoreRotatedCoords();//restores the previous state of the rotated dest coords

  return succeeded;
}

//********************************************************************************************************
// YV12 Texture creation, deletion, copying + clearing
//********************************************************************************************************
void CWinRenderer::DeleteRenderBuffer(int index)
{
  CSingleLock lock(g_graphicsContext);
  ReleaseBuffer(index);
  m_renderBuffers[index].Release();
}

bool CWinRenderer::CreateRenderBuffer(int index)
{
  CSingleLock lock(g_graphicsContext);
  DeleteRenderBuffer(index);

  if (!m_renderBuffers[index].CreateBuffer(m_bufferFormat, m_sourceWidth, m_sourceHeight, m_renderMethod == RENDER_SW))
  {
    CLog::Log(LOGERROR, "%s: unable to create video buffer %i", __FUNCTION__, index);
    m_renderBuffers[index].Release();
    return false;
  }
  m_renderBuffers[index].Lock();
  m_renderBuffers[index].Clear();

  CLog::Log(LOGDEBUG, "%s: created video buffer %i", __FUNCTION__, index);
  return true;
}

bool CWinRenderer::Supports(ERENDERFEATURE feature)
{
  if(feature == RENDERFEATURE_BRIGHTNESS)
    return true;

  if(feature == RENDERFEATURE_CONTRAST)
    return true;

  if (feature == RENDERFEATURE_STRETCH         ||
      feature == RENDERFEATURE_NONLINSTRETCH   ||
      feature == RENDERFEATURE_ZOOM            ||
      feature == RENDERFEATURE_VERTICAL_SHIFT  ||
      feature == RENDERFEATURE_PIXEL_RATIO     ||
      feature == RENDERFEATURE_ROTATION        ||
      feature == RENDERFEATURE_POSTPROCESS)
    return true;

  return false;
}

bool CWinRenderer::Supports(ESCALINGMETHOD method)
{
  if (m_renderMethod == RENDER_PS || m_renderMethod == RENDER_DXVA)
  {
    if (m_renderMethod == RENDER_DXVA)
    {
      if (method == VS_SCALINGMETHOD_DXVA_HARDWARE 
       || method == VS_SCALINGMETHOD_AUTO)
        return true;
      if (!g_advancedSettings.m_DXVAAllowHqScaling || m_renderOrientation)
        return false;
    }

    if ( method == VS_SCALINGMETHOD_AUTO
     || (method == VS_SCALINGMETHOD_LINEAR && m_renderMethod == RENDER_PS)) 
        return true;

    if (g_Windowing.GetFeatureLevel() >= D3D_FEATURE_LEVEL_9_3 && !m_renderOrientation)
    {
      if (method == VS_SCALINGMETHOD_CUBIC
       || method == VS_SCALINGMETHOD_LANCZOS2
       || method == VS_SCALINGMETHOD_SPLINE36_FAST
       || method == VS_SCALINGMETHOD_LANCZOS3_FAST
       || method == VS_SCALINGMETHOD_SPLINE36
       || method == VS_SCALINGMETHOD_LANCZOS3)
      {
        // if scaling is below level, avoid hq scaling
        float scaleX = fabs((static_cast<float>(m_sourceWidth) - m_destRect.Width())/m_sourceWidth)*100;
        float scaleY = fabs((static_cast<float>(m_sourceHeight) - m_destRect.Height())/m_sourceHeight)*100;
        int minScale = CServiceBroker::GetSettings().GetInt(CSettings::SETTING_VIDEOPLAYER_HQSCALERS);
        if (scaleX < minScale && scaleY < minScale)
          return false;
        return true;
      }
    }
  }
  else if(m_renderMethod == RENDER_SW)
  {
    if (method == VS_SCALINGMETHOD_AUTO
     || method == VS_SCALINGMETHOD_LINEAR)
      return true;
  }
  return false;
}

bool CWinRenderer::WantsDoublePass()
{
  if (m_renderMethod == RENDER_DXVA)
    return true;

  return false;
}

bool CWinRenderer::ConfigChanged(const VideoPicture& picture)
{
  if (picture.videoBuffer->GetFormat() != m_format)
    return true;

  return false;
}

bool CWinRenderer::HandlesVideoBuffer(CVideoBuffer* buffer)
{
  AVPixelFormat format = buffer->GetFormat();
  if ( format == AV_PIX_FMT_D3D11VA_VLD
    || format == AV_PIX_FMT_NV12
    || format == AV_PIX_FMT_P010
    || format == AV_PIX_FMT_P016
    || format == AV_PIX_FMT_YUV420P
    || format == AV_PIX_FMT_YUV420P10
    || format == AV_PIX_FMT_YUV420P16)
    return true;

  return false;
}

CRenderInfo CWinRenderer::GetRenderInfo()
{
  CRenderInfo info;
  info.formats = 
  { 
    AV_PIX_FMT_D3D11VA_VLD, 
    AV_PIX_FMT_NV12, 
    AV_PIX_FMT_P010, 
    AV_PIX_FMT_P016, 
    AV_PIX_FMT_YUV420P, 
    AV_PIX_FMT_YUV420P10, 
    AV_PIX_FMT_YUV420P16 
  };
  info.max_buffer_size = NUM_BUFFERS;
  if (m_renderMethod == RENDER_DXVA && m_processor)
  {
    int buffers = m_processor->Size() + m_processor->PastRefs(); // extra buffers for past refs
    info.optimal_buffer_size = std::min(NUM_BUFFERS, buffers);
    if (m_format != AV_PIX_FMT_D3D11VA_VLD)
      info.m_deintMethods.push_back(VS_INTERLACEMETHOD_DXVA_AUTO);
  }
  else
    info.optimal_buffer_size = 4;
  return info;
}

void CWinRenderer::ReleaseBuffer(int idx)
{
  SAFE_RELEASE(m_renderBuffers[idx].videoBuffer);
}

bool CWinRenderer::NeedBuffer(int idx)
{
  // check if processor wants to keep past frames
  if (m_renderMethod == RENDER_DXVA && m_processor)
  {
    int numPast = m_processor->PastRefs();
    if (m_renderBuffers[idx].HasPic())
    {
      if (m_renderBuffers[idx].frameIdx + numPast*2 >= m_renderBuffers[m_iYV12RenderBuffer].frameIdx)
        return true;
    }
  }
  return false;
}

// Color management helpers

void CWinRenderer::ColorManagmentUpdate()
{
  if (m_colorManager->IsEnabled())
  {
    if (!m_colorManager->CheckConfiguration(m_cmsToken, m_iFlags))
    {
      CLog::Log(LOGDEBUG, "%s: CMS configuration changed, reload LUT", __FUNCTION__);
      LoadCLUT();
    }
    m_cmsOn = true;
  }
  else
  {
    m_cmsOn = false;
  }
}

bool CWinRenderer::LoadCLUT()
{
  m_CLUTSize = 0;
  m_clutLoaded = false;

  auto loadLutTask = Concurrency::create_task([this]{
    // load 3DLUT data
    int clutSize, dataSize;
    if (!CColorManager::Get3dLutSize(CMS_DATA_FMT_RGBA, &clutSize, &dataSize))
      return 0;

    uint16_t* clutData = static_cast<uint16_t*>(_aligned_malloc(dataSize, 16));
    bool success = m_colorManager->GetVideo3dLut(m_iFlags, &m_cmsToken, CMS_DATA_FMT_RGBA, clutSize, clutData);
    if (success)
    {
      SAFE_RELEASE(m_pCLUTView);
      success = COutputShader::CreateCLUTView(clutSize, clutData, false, &m_pCLUTView);
    }
    else
      CLog::Log(LOGERROR, "%s: unable to loading the 3dlut data.", __FUNCTION__);

    _aligned_free(clutData);
    if (!success)
      return 0;

    return clutSize;
  });

  loadLutTask.then([&](int clutSize){
    m_CLUTSize = clutSize;
    if (m_outputShader)
        m_outputShader->SetCLUT(m_CLUTSize, m_pCLUTView);
    m_clutLoaded = true;
  });
  return true;
}
