#include "hooking.h"
#include "DDSTextureLoader11.h"
#include "WICTextureLoader11.h"
#include <MinHook.h>
#include <REX/W32/COMPTR.h>
#include <Shlwapi.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3dcommon.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <limits>
#include <vector>

#include "Settings.h"
#include "ReticleVertexScaling.h"
#include "WorldOnlyScopeRenderer.h"
#include "hookingStruct.h"
#include <MathUtils.h>
#include <renderdoc_app.h>

#include <d3d9.h>

#pragma comment(lib, "D3DCompiler.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "shlwapi.lib")

bool EnableDebugPrivilege()
{
	HANDLE hToken;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
		return false;
	}

	LUID luid;
	if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &luid)) {
		CloseHandle(hToken);
		return false;
	}

	TOKEN_PRIVILEGES tp;
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = luid;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

	if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL)) {
		CloseHandle(hToken);
		return false;
	}

	CloseHandle(hToken);
	return GetLastError() == ERROR_SUCCESS;
}

bool IsExecutableAddress(const void* address)
{
	if (!address) {
		return false;
	}
	MEMORY_BASIC_INFORMATION memory{};
	if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
		memory.State != MEM_COMMIT ||
		(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
		return false;
	}
	const DWORD executableProtection =
		PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	return (memory.Protect & executableProtection) != 0;
}

#ifdef _DEBUG
RENDERDOC_API_1_6_0* rdoc_api = nullptr;
#endif
static constexpr UINT TARGET_STRIDE = 20;
static constexpr UINT TARGET_INDEX_COUNT = 24;
static constexpr UINT TARGET_BUFFER_SIZE = 0x0000000008000000;
static constexpr UINT TARGET_TEXTURE_WIDTH = 2048;
static constexpr UINT TARGET_TEXTURE_HEIGHT = 2048;
static constexpr DXGI_FORMAT TARGET_TEXTURE_FORMAT = DXGI_FORMAT_BC2_UNORM_SRGB;

ComPtr<IDXGISwapChain> g_Swapchain = nullptr;
ComPtr<ID3D11Device> g_Device = nullptr;
ComPtr<ID3D11DeviceContext> g_Context = nullptr;

namespace
{
	bool HaveSameCOMIdentity(IUnknown* left, IUnknown* right)
	{
		if (!left || !right) {
			return false;
		}
		if (left == right) {
			return true;
		}

		// COM permits distinct interface pointers for one object. Comparing
		// the controlling IUnknown is the defined identity test and avoids
		// treating an ENB or upscaler device wrapper as a second D3D device
		// merely because its ID3D11Device interface address differs.
		IUnknown* leftIdentity = nullptr;
		IUnknown* rightIdentity = nullptr;
		const HRESULT leftResult = left->QueryInterface(
			IID_IUnknown,
			reinterpret_cast<void**>(&leftIdentity));
		const HRESULT rightResult = right->QueryInterface(
			IID_IUnknown,
			reinterpret_cast<void**>(&rightIdentity));
		const bool same =
			SUCCEEDED(leftResult) &&
			SUCCEEDED(rightResult) &&
			leftIdentity == rightIdentity;
		SAFE_RELEASE(leftIdentity);
		SAFE_RELEASE(rightIdentity);
		return same;
	}

	struct ReticleGeometrySnapshot
	{
		std::uint64_t generation = 0U;
		std::uint32_t vertexCount = 0U;
		std::uint32_t indexCount = 0U;
		std::uint32_t stride = 0U;
		std::uint32_t vertexDataOffset = 0U;
		std::uint32_t indexDataOffset = 0U;
		std::uint64_t vertexDescriptor = 0U;
	};

	bool ReadReticleGeometrySnapshot(ReticleGeometrySnapshot& result)
	{
		const auto generationBefore =
			Hook::D3D::automaticSTSReticleGeometryGeneration.load(
				std::memory_order_acquire);
		if (!Hook::D3D::automaticSTSReticleGeometryReady.load(
				std::memory_order_acquire)) {
			return false;
		}

		ReticleGeometrySnapshot snapshot{};
		snapshot.generation = generationBefore;
		snapshot.vertexCount =
			Hook::D3D::automaticSTSReticleVertexCount.load(
				std::memory_order_relaxed);
		snapshot.indexCount =
			Hook::D3D::automaticSTSReticleIndexCount.load(
				std::memory_order_relaxed);
		snapshot.stride =
			Hook::D3D::automaticSTSReticleVertexStride.load(
				std::memory_order_relaxed);
		snapshot.vertexDataOffset =
			Hook::D3D::automaticSTSReticleVertexDataOffset.load(
				std::memory_order_relaxed);
		snapshot.indexDataOffset =
			Hook::D3D::automaticSTSReticleIndexDataOffset.load(
				std::memory_order_relaxed);
		snapshot.vertexDescriptor =
			Hook::D3D::automaticSTSReticleVertexDescriptor.load(
				std::memory_order_relaxed);

		const auto generationAfter =
			Hook::D3D::automaticSTSReticleGeometryGeneration.load(
				std::memory_order_acquire);
		if (generationBefore != generationAfter ||
			!Hook::D3D::automaticSTSReticleGeometryReady.load(
				std::memory_order_acquire)) {
			return false;
		}
		result = snapshot;
		return true;
	}

	bool MatchesAutomaticSTSReticleSet(
		ID3D11Buffer* vertexBuffer,
		ID3D11Buffer* indexBuffer,
		std::uint32_t indexCount,
		std::uint32_t vertexStride,
		std::uint32_t vertexOffset,
		std::int64_t effectiveVertexOffset,
		std::uint32_t indexOffset,
		std::uint64_t effectiveIndexOffset,
		bool knownIndexFormat)
	{
		if (!vertexBuffer || !indexBuffer || !knownIndexFormat) {
			return false;
		}

		const auto vertexAddress =
			reinterpret_cast<std::uintptr_t>(vertexBuffer);
		const auto indexAddress =
			reinterpret_cast<std::uintptr_t>(indexBuffer);
		// Equipment publication is infrequent, while this reader runs for every
		// draw. Two bounded retries avoid accepting a mixed reticle set without
		// putting a mutex or scene pointer on Fallout's render thread.
		for (std::uint32_t attempt = 0U; attempt < 2U; ++attempt) {
			const auto sequenceBefore =
				Hook::D3D::automaticSTSReticleSetSequence.load(
					std::memory_order_acquire);
			if ((sequenceBefore & 1U) != 0U) {
				continue;
			}
			const auto count = std::min(
				Hook::D3D::automaticSTSReticleGeometryCount.load(
					std::memory_order_acquire),
				static_cast<std::uint32_t>(
					Hook::D3D::kMaxAutomaticSTSReticleGeometries));
			bool matched = false;
			for (std::uint32_t index = 0U; index < count; ++index) {
				const auto& identity =
					Hook::D3D::automaticSTSReticleGeometries[index];
				const auto expectedVertexOffset =
					identity.vertexDataOffset.load(std::memory_order_relaxed);
				const auto expectedIndexOffset =
					identity.indexDataOffset.load(std::memory_order_relaxed);
				matched =
					identity.vertexBuffer.load(std::memory_order_relaxed) ==
						vertexAddress &&
					identity.indexBuffer.load(std::memory_order_relaxed) ==
						indexAddress &&
					identity.indexCount.load(std::memory_order_relaxed) ==
						indexCount &&
					identity.vertexStride.load(std::memory_order_relaxed) ==
						vertexStride &&
					(vertexOffset == expectedVertexOffset ||
						effectiveVertexOffset ==
							static_cast<std::int64_t>(expectedVertexOffset)) &&
					(indexOffset == expectedIndexOffset ||
						effectiveIndexOffset == expectedIndexOffset);
				if (matched) {
					break;
				}
			}
			const auto sequenceAfter =
				Hook::D3D::automaticSTSReticleSetSequence.load(
					std::memory_order_acquire);
			if (sequenceBefore == sequenceAfter &&
				(sequenceAfter & 1U) == 0U) {
				return matched;
			}
		}
		return false;
	}

	struct ReticleScaleCache
	{
		enum class State
		{
			kEmpty,
			kPending,
			kReady,
			kFailed
		};

		State state = State::kEmpty;
		ComPtr<ID3D11Device> device;
		ID3D11DeviceContext* contextIdentity = nullptr;
		ID3D11Buffer* sourceVertexBuffer = nullptr;
		ID3D11Buffer* sourceIndexBuffer = nullptr;
		ReticleGeometrySnapshot geometry{};
		DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
		std::uint32_t vertexByteOffset = 0U;
		std::uint32_t indexByteOffset = 0U;
		ComPtr<ID3D11Buffer> vertexReadback;
		ComPtr<ID3D11Buffer> indexReadback;
		ComPtr<ID3D11Buffer> scaledVertexBuffer;
		ComPtr<ID3D11Query> copyComplete;
		MagnaScope::ReticleVertexScaling::PreparedVertices prepared;
		std::vector<std::byte> scaledBytes;
		float uploadedScale = std::numeric_limits<float>::quiet_NaN();
		bool retryableFailure = false;
		std::uint32_t retryCountdown = 0U;

		void Reset()
		{
			*this = {};
		}

		void FailPermanently()
		{
			// Invalid topology, descriptors, or authored data will not become
			// valid on a later frame. Latch those failures until the published
			// geometry generation changes so a malformed reticle cannot cause
			// repeated GPU readbacks every draw.
			state = State::kFailed;
			retryableFailure = false;
			retryCountdown = 0U;
		}

		void FailTransiently()
		{
			// Device allocation, query, and Map failures may be temporary
			// (device pressure, removal/recreation, or a busy immediate
			// context). Keep the authored draw for one second at 60 Hz, then
			// retry the asynchronous preparation without blocking the render
			// thread or permanently disabling this reticle.
			state = State::kFailed;
			retryableFailure = true;
			retryCountdown = 60U;
		}

		[[nodiscard]] bool Matches(
			ID3D11Device* currentDevice,
			ID3D11DeviceContext* currentContext,
			ID3D11Buffer* vertexBuffer,
			ID3D11Buffer* indexBuffer,
			const ReticleGeometrySnapshot& currentGeometry,
			DXGI_FORMAT currentIndexFormat,
			std::uint32_t currentVertexByteOffset,
			std::uint32_t currentIndexByteOffset) const
		{
			return state != State::kEmpty &&
			       HaveSameCOMIdentity(device.Get(), currentDevice) &&
			       contextIdentity == currentContext &&
			       sourceVertexBuffer == vertexBuffer &&
			       sourceIndexBuffer == indexBuffer &&
			       geometry.generation == currentGeometry.generation &&
			       geometry.vertexCount == currentGeometry.vertexCount &&
			       geometry.indexCount == currentGeometry.indexCount &&
			       geometry.stride == currentGeometry.stride &&
			       geometry.vertexDescriptor ==
			           currentGeometry.vertexDescriptor &&
			       indexFormat == currentIndexFormat &&
			       vertexByteOffset == currentVertexByteOffset &&
			       indexByteOffset == currentIndexByteOffset;
		}

		bool Begin(
			ID3D11Device* currentDevice,
			ID3D11DeviceContext* currentContext,
			ID3D11Buffer* vertexBuffer,
			ID3D11Buffer* indexBuffer,
			const ReticleGeometrySnapshot& currentGeometry,
			DXGI_FORMAT currentIndexFormat,
			std::uint32_t currentVertexByteOffset,
			std::uint32_t currentIndexByteOffset)
		{
			Reset();
			device = currentDevice;
			contextIdentity = currentContext;
			sourceVertexBuffer = vertexBuffer;
			sourceIndexBuffer = indexBuffer;
			geometry = currentGeometry;
			indexFormat = currentIndexFormat;
			vertexByteOffset = currentVertexByteOffset;
			indexByteOffset = currentIndexByteOffset;
			if (!currentDevice ||
				!currentContext ||
				!vertexBuffer ||
				!indexBuffer ||
				currentContext->GetType() !=
					D3D11_DEVICE_CONTEXT_IMMEDIATE ||
				currentGeometry.vertexCount == 0U ||
				currentGeometry.vertexCount > 65535U ||
				currentGeometry.indexCount == 0U ||
				currentGeometry.stride < sizeof(std::uint16_t) * 3U ||
				currentGeometry.stride > 512U ||
				(currentIndexFormat != DXGI_FORMAT_R16_UINT &&
					currentIndexFormat != DXGI_FORMAT_R32_UINT)) {
				FailPermanently();
				return false;
			}

			const std::uint32_t indexElementSize =
				currentIndexFormat == DXGI_FORMAT_R32_UINT ? 4U : 2U;
			const std::uint64_t vertexBytes64 =
				static_cast<std::uint64_t>(currentGeometry.vertexCount) *
				currentGeometry.stride;
			const std::uint64_t indexBytes64 =
				static_cast<std::uint64_t>(currentGeometry.indexCount) *
				indexElementSize;
			constexpr std::uint64_t kMaximumReticleBytes = 16U * 1024U * 1024U;
			if (vertexBytes64 == 0U ||
				indexBytes64 == 0U ||
				vertexBytes64 > kMaximumReticleBytes ||
				indexBytes64 > kMaximumReticleBytes ||
				vertexBytes64 >
					std::numeric_limits<std::uint32_t>::max() ||
				indexBytes64 >
					std::numeric_limits<std::uint32_t>::max()) {
				FailPermanently();
				return false;
			}

			D3D11_BUFFER_DESC sourceVertexDescription{};
			D3D11_BUFFER_DESC sourceIndexDescription{};
			vertexBuffer->GetDesc(&sourceVertexDescription);
			indexBuffer->GetDesc(&sourceIndexDescription);
			if (currentVertexByteOffset >
					sourceVertexDescription.ByteWidth ||
				vertexBytes64 >
					sourceVertexDescription.ByteWidth -
						currentVertexByteOffset ||
				currentIndexByteOffset >
					sourceIndexDescription.ByteWidth ||
				indexBytes64 >
					sourceIndexDescription.ByteWidth -
						currentIndexByteOffset) {
				FailPermanently();
				return false;
			}

			D3D11_BUFFER_DESC readbackDescription{};
			readbackDescription.Usage = D3D11_USAGE_STAGING;
			readbackDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			readbackDescription.ByteWidth =
				static_cast<UINT>(vertexBytes64);
			if (FAILED(currentDevice->CreateBuffer(
					&readbackDescription,
					nullptr,
					vertexReadback.GetAddressOf()))) {
				FailTransiently();
				return false;
			}
			readbackDescription.ByteWidth =
				static_cast<UINT>(indexBytes64);
			if (FAILED(currentDevice->CreateBuffer(
					&readbackDescription,
					nullptr,
					indexReadback.GetAddressOf()))) {
				FailTransiently();
				return false;
			}

			D3D11_BUFFER_DESC scaledDescription{};
			scaledDescription.ByteWidth = static_cast<UINT>(vertexBytes64);
			scaledDescription.Usage = D3D11_USAGE_DYNAMIC;
			scaledDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			scaledDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(currentDevice->CreateBuffer(
					&scaledDescription,
					nullptr,
					scaledVertexBuffer.GetAddressOf()))) {
				FailTransiently();
				return false;
			}

			D3D11_QUERY_DESC queryDescription{};
			queryDescription.Query = D3D11_QUERY_EVENT;
			if (FAILED(currentDevice->CreateQuery(
					&queryDescription,
					copyComplete.GetAddressOf()))) {
				FailTransiently();
				return false;
			}

			const D3D11_BOX vertexBox{
				currentVertexByteOffset,
				0U,
				0U,
				static_cast<UINT>(
					currentVertexByteOffset + vertexBytes64),
				1U,
				1U
			};
			const D3D11_BOX indexBox{
				currentIndexByteOffset,
				0U,
				0U,
				static_cast<UINT>(
					currentIndexByteOffset + indexBytes64),
				1U,
				1U
			};
			currentContext->CopySubresourceRegion(
				vertexReadback.Get(),
				0U,
				0U,
				0U,
				0U,
				vertexBuffer,
				0U,
				&vertexBox);
			currentContext->CopySubresourceRegion(
				indexReadback.Get(),
				0U,
				0U,
				0U,
				0U,
				indexBuffer,
				0U,
				&indexBox);
			currentContext->End(copyComplete.Get());

			state = State::kPending;
			return true;
		}

		bool Poll(ID3D11DeviceContext* context)
		{
			if (state != State::kPending ||
				!context ||
				context != contextIdentity ||
				!copyComplete.Get() ||
				!vertexReadback.Get() ||
				!indexReadback.Get()) {
				return state == State::kReady;
			}

			const HRESULT queryResult = context->GetData(
				copyComplete.Get(),
				nullptr,
				0U,
				D3D11_ASYNC_GETDATA_DONOTFLUSH);
			if (queryResult == S_FALSE) {
				return false;
			}
			if (FAILED(queryResult)) {
				FailTransiently();
				return false;
			}

			D3D11_MAPPED_SUBRESOURCE mappedVertex{};
			D3D11_MAPPED_SUBRESOURCE mappedIndex{};
			if (FAILED(context->Map(
					vertexReadback.Get(),
					0U,
					D3D11_MAP_READ,
					0U,
					&mappedVertex))) {
				FailTransiently();
				return false;
			}
			const std::size_t vertexByteCount =
				static_cast<std::size_t>(geometry.vertexCount) *
				geometry.stride;
			std::vector<std::byte> authored(vertexByteCount);
			std::memcpy(
				authored.data(),
				mappedVertex.pData,
				vertexByteCount);
			context->Unmap(vertexReadback.Get(), 0U);

			if (FAILED(context->Map(
					indexReadback.Get(),
					0U,
					D3D11_MAP_READ,
					0U,
					&mappedIndex))) {
				FailTransiently();
				return false;
			}
			const std::uint32_t indexElementSize =
				indexFormat == DXGI_FORMAT_R32_UINT ? 4U : 2U;
			const std::size_t indexByteCount =
				static_cast<std::size_t>(geometry.indexCount) *
				indexElementSize;
			std::vector<std::byte> indices(indexByteCount);
			std::memcpy(
				indices.data(),
				mappedIndex.pData,
				indexByteCount);
			context->Unmap(indexReadback.Get(), 0U);

			if (!MagnaScope::ReticleVertexScaling::ValidateIndices(
					indices,
					geometry.indexCount,
					indexElementSize,
					geometry.vertexCount) ||
				!MagnaScope::ReticleVertexScaling::Prepare(
					authored,
					geometry.vertexCount,
					geometry.stride,
					geometry.vertexDescriptor,
					prepared)) {
				FailPermanently();
				return false;
			}

			vertexReadback.Reset();
			indexReadback.Reset();
			copyComplete.Reset();
			state = State::kReady;
			logger::info(
				"Prepared STS reticle vertex scaling: vertices={}, "
				"stride={}, center=({:.4f}, {:.4f}, {:.4f}), "
				"encoding={}",
				prepared.vertexCount,
				prepared.stride,
				prepared.centroid.x,
				prepared.centroid.y,
				prepared.centroid.z,
				prepared.fullPrecision ? "float3" : "fp16x3");
			return true;
		}

		bool Upload(ID3D11DeviceContext* context, float scale)
		{
			if (state != State::kReady ||
				!context ||
				!scaledVertexBuffer.Get() ||
				!std::isfinite(scale)) {
				return false;
			}
			if (std::isfinite(uploadedScale) &&
				std::abs(uploadedScale - scale) <= 0.00001F) {
				return true;
			}
			if (!MagnaScope::ReticleVertexScaling::Scale(
					prepared,
					scale,
					scaledBytes)) {
				FailPermanently();
				return false;
			}

			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(
					scaledVertexBuffer.Get(),
					0U,
					D3D11_MAP_WRITE_DISCARD,
					0U,
					&mapped))) {
				FailTransiently();
				return false;
			}
			std::memcpy(
				mapped.pData,
				scaledBytes.data(),
				scaledBytes.size());
			context->Unmap(scaledVertexBuffer.Get(), 0U);
			uploadedScale = scale;
			return true;
		}
	};

	template <class Draw>
	bool DrawReticleWithScaledVertices(
		ID3D11DeviceContext* context,
		ID3D11Buffer* currentVertexBuffer,
		ID3D11Buffer* currentIndexBuffer,
		UINT currentStride,
		UINT currentVertexOffset,
		DXGI_FORMAT currentIndexFormat,
		UINT currentIndexOffset,
		UINT startIndexLocation,
		INT baseVertexLocation,
		Draw&& draw)
	{
		if (!context ||
			!currentVertexBuffer ||
			!currentIndexBuffer) {
			return false;
		}

		const float requestedScale = std::clamp(
			Hook::D3D::scopeReticleMagnification.load(
				std::memory_order_acquire),
			0.25F,
			8.0F);
		const float requestedSceneMagnification = std::clamp(
			Hook::D3D::scopeFadeMagnification.load(
				std::memory_order_acquire),
			1.0F,
			15.0F);
		const float activation = std::clamp(
			Hook::D3D::projectedActivationProgress.load(
				std::memory_order_acquire),
			0.0F,
			1.0F);
		// Keep the reticle in Fallout's authored material pass. The later
		// ScopeFade composite magnifies every source pixel inside the aperture,
		// including this reticle, so shrink its geometry by the reciprocal first.
		// The two transforms cancel at the default 1x reticle setting while an
		// explicit reticle scale remains independent of scene magnification.
		const float scale =
			MagnaScope::ReticleVertexScaling::CalculateAuthoredPreScale(
				requestedScale,
				requestedSceneMagnification,
				activation);
		if (std::abs(scale - 1.0F) <= 0.0001F ||
			!std::isfinite(scale)) {
			return false;
		}

		ReticleGeometrySnapshot geometry{};
		if (!ReadReticleGeometrySnapshot(geometry) ||
			geometry.stride != currentStride ||
			baseVertexLocation < 0) {
			return false;
		}
		const std::uint64_t vertexByteOffset64 =
			static_cast<std::uint64_t>(currentVertexOffset) +
			static_cast<std::uint64_t>(baseVertexLocation) *
				currentStride;
		const std::uint32_t indexElementSize =
			currentIndexFormat == DXGI_FORMAT_R32_UINT ? 4U :
			currentIndexFormat == DXGI_FORMAT_R16_UINT ? 2U :
				0U;
		const std::uint64_t indexByteOffset64 =
			static_cast<std::uint64_t>(currentIndexOffset) +
			static_cast<std::uint64_t>(startIndexLocation) *
				indexElementSize;
		if (indexElementSize == 0U ||
			vertexByteOffset64 != geometry.vertexDataOffset ||
			indexByteOffset64 != geometry.indexDataOffset ||
			vertexByteOffset64 >
				std::numeric_limits<std::uint32_t>::max() ||
			indexByteOffset64 >
				std::numeric_limits<std::uint32_t>::max()) {
			return false;
		}

		ComPtr<ID3D11Device> device;
		context->GetDevice(device.GetAddressOf());
		if (!device.Get()) {
			return false;
		}

		thread_local ReticleScaleCache cache;
		const bool cacheMatches = cache.Matches(
				device.Get(),
				context,
				currentVertexBuffer,
				currentIndexBuffer,
				geometry,
				currentIndexFormat,
				static_cast<std::uint32_t>(vertexByteOffset64),
				static_cast<std::uint32_t>(indexByteOffset64));
		if (cacheMatches &&
			cache.state == ReticleScaleCache::State::kFailed &&
			cache.retryableFailure) {
			if (cache.retryCountdown > 0U) {
				--cache.retryCountdown;
				return false;
			}
			// Reset before Begin so Matches cannot keep a transient failure
			// latched forever. The retry remains asynchronous and this frame
			// still renders the authored STS reticle.
			cache.Reset();
		}
		if (!cacheMatches ||
			cache.state == ReticleScaleCache::State::kEmpty) {
			cache.Begin(
				device.Get(),
				context,
				currentVertexBuffer,
				currentIndexBuffer,
				geometry,
				currentIndexFormat,
				static_cast<std::uint32_t>(vertexByteOffset64),
				static_cast<std::uint32_t>(indexByteOffset64));
			return false;
		}
		if (!cache.Poll(context) ||
			!cache.Upload(context, scale)) {
			return false;
		}

		ID3D11Buffer* scaledBuffer = cache.scaledVertexBuffer.Get();
		const UINT scaledOffset = 0U;
		context->IASetVertexBuffers(
			0U,
			1U,
			&scaledBuffer,
			&currentStride,
			&scaledOffset);
		draw(0);
		context->IASetVertexBuffers(
			0U,
			1U,
			&currentVertexBuffer,
			&currentStride,
			&currentVertexOffset);
		return true;
	}
}

bool bChangeAimTexture = true;
bool bResetZoomDelta = false;
// Hook-generated draws can re-enter DrawIndexed on the same thread. Keeping
// this guard thread-local prevents an unrelated deferred/context thread from
// accidentally bypassing capture while the immediate context is replaying.
thread_local bool bSelfDraw = false;
// Retained so Present can compare what we hooked against what is in the vtable
// now. An upscaler proxy or another integration replacing the entry after us
// silently removes our detour from the chain, and the only visible symptom is
// a draw count far below the frame's real one.
DWORD_PTR* g_deviceContextVTable = nullptr;
struct DrawHookBinding;
extern DrawHookBinding g_drawIndexedBinding;
extern DrawHookBinding g_drawIndexedInstancedBinding;

// "module.dll+0xOFFSET" for a code address. Which module owns a hook target is
// the difference between having hooked d3d11's own function and having hooked
// somebody else's detour or a proxy context's vtable thunk.
std::string DescribeCodeAddress(const void* address)
{
	if (!address) {
		return "<null>";
	}
	HMODULE module = nullptr;
	if (!GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(address),
			&module) ||
		!module) {
		return "<unowned>";
	}
	char path[MAX_PATH]{};
	if (GetModuleFileNameA(module, path, static_cast<DWORD>(std::size(path))) ==
		0U) {
		return "<unnamed>";
	}
	const char* name = std::strrchr(path, '\\');
	name = name ? name + 1 : path;
	const auto offset =
		reinterpret_cast<std::uintptr_t>(address) -
		reinterpret_cast<std::uintptr_t>(module);
	return std::format("{}+0x{:X}", name, offset);
}

// Keep a draw entry hooked no matter which implementation the vtable points at.
//
// MinHook patches a function's prologue, not the vtable slot, so hooking the
// address a slot happens to contain is only correct while it keeps pointing
// there. It does not. Fallout's device context alternates between d3d11's own
// DrawIndexed and a wrapper another mod installs -- d3d11+0x1545A0 against
// ShaderEngineCL+0x67520 -- switching several times a second.
//
// Rebinding to whichever address the slot currently holds cannot win that: it
// is a race with no finish line. An earlier attempt burned its whole sixteen
// rebind budget in five seconds and then sat on whichever implementation it had
// last patched, which was the live one about half the time. That is exactly the
// coin flip this bug always presented as.
//
// So hook both and never unhook. Each target gets its own detour and its own
// trampoline, because a shared detour cannot tell which one it was entered
// through. Whichever implementation the context calls, one of ours runs.
struct DrawHookBinding
{
	void* primaryTarget = nullptr;
	void* alternateTarget = nullptr;
	bool exhausted = false;
};

DrawHookBinding g_drawIndexedBinding{};
DrawHookBinding g_drawIndexedInstancedBinding{};

bool BindDrawHookTarget(
	DWORD_PTR* vtable,
	std::size_t index,
	void* primaryDetour,
	void** primaryOriginal,
	void* alternateDetour,
	void** alternateOriginal,
	DrawHookBinding& binding,
	const char* hookName)
{
	auto* const current = reinterpret_cast<void*>(vtable[index]);
	if (!current || current == binding.primaryTarget ||
		current == binding.alternateTarget || current == primaryDetour ||
		current == alternateDetour || !IsExecutableAddress(current)) {
		return false;
	}

	void* detour = nullptr;
	void** original = nullptr;
	void** slot = nullptr;
	if (!binding.primaryTarget) {
		detour = primaryDetour;
		original = primaryOriginal;
		slot = &binding.primaryTarget;
	} else if (!binding.alternateTarget) {
		detour = alternateDetour;
		original = alternateOriginal;
		slot = &binding.alternateTarget;
	} else {
		// Two implementations is what this game presents. A third would mean
		// the assumption is wrong; say so once rather than thrash.
		if (!binding.exhausted) {
			binding.exhausted = true;
			logger::error(
				"{} saw a third implementation at {:p} ({}); only two can be "
				"hooked and draws through this one will be missed",
				hookName,
				current,
				DescribeCodeAddress(current));
		}
		return false;
	}

	const auto created = MH_CreateHook(current, detour, original);
	if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED) {
		logger::error(
			"Failed to hook {} implementation at {:p} ({})",
			hookName,
			current,
			DescribeCodeAddress(current));
		return false;
	}
	if (MH_EnableHook(current) != MH_OK) {
		logger::error(
			"Failed to enable {} implementation at {:p} ({})",
			hookName,
			current,
			DescribeCodeAddress(current));
		return false;
	}
	*slot = current;
	logger::info(
		"Hooked {} implementation at {:p} ({})",
		hookName,
		current,
		DescribeCodeAddress(current));
	return true;
}

bool isActive_TAA = false;
bool isActive_DOF = false;
bool renderedAtTAAThisFrame = false;
bool compositedThisFrame = false;
bool lastRenderProducedComposite = false;
bool renderPassHandledThisFrame = false;
// ScopeFade is captured during Fallout's first-person draw and consumed later
// in the same presented frame. A monotonically increasing epoch prevents a
// missing draw from replaying the previous frame's weapon transform.
std::atomic_uint64_t automaticSTSReplayFrameGeneration = 1U;
// Stage 3 callbacks can execute on different engine threads. This atomic is a
// narrow handoff only: it tells the Menu Framework frame boundary that the
// guarded TAA callback already captured a source. No game object or profile
// pointer crosses through it.
std::atomic_bool taaVerificationCapturedSinceFramework = false;

using namespace DirectX;

static HWND g_hWnd;
static HMODULE g_hModule;

DWORD_PTR* pSwapChainVTable = nullptr;
DWORD_PTR* pDeviceVTable = nullptr;
DWORD_PTR* pDeviceContextVTable = nullptr;

int windowWidth;
int windowHeight;

float gameZoomDelta = 1;
bool bIsFirst = true;

ID3D11Buffer* targetVertexConstBuffer;
ID3D11Buffer* targetVertexConstBuffer1p5;
ID3D11Buffer* targetVertexConstBuffer1;

ID3D11Buffer* targetVertexConstBufferOutPut;
ID3D11Buffer* targetVertexConstBufferOutPut1p5;
ID3D11Buffer* targetVertexConstBufferOutPut1;

Microsoft::WRL::ComPtr<ID3D11Buffer> targetIndexBuffer;
DXGI_FORMAT targetIndexBufferFormat;
UINT targetIndexBufferOffset;

bool bDrawIndexed = true;

std::atomic<IDXGISwapChain*> lastPresentedSwapChain{ nullptr };
IDXGISwapChain* backBufferOwnerSwapChain = nullptr;
UINT mBackBufferIndex = UINT_MAX;
bool getBufferReferenceContractProbed = false;
bool getBufferAddsReference = true;

ID3D11Texture2D* AcquireBackBuffer(IDXGISwapChain* swapChain, UINT index)
{
	if (!swapChain) {
		return nullptr;
	}

	ID3D11Texture2D* texture = nullptr;
	if (FAILED(swapChain->GetBuffer(index, IID_PPV_ARGS(&texture))) || !texture) {
		return nullptr;
	}

	if (!getBufferReferenceContractProbed) {
		getBufferReferenceContractProbed = true;

		// Standard DXGI increments the returned texture's reference count.
		// Some frame-generation/upscaler proxy chains return their internal
		// pointer without doing so. Probe the contract once, matching F4SE
		// Menu Framework's proven handling, so MagnaScope never releases a
		// reference that it did not acquire.
		texture->AddRef();
		const ULONG firstCount = texture->Release();

		ID3D11Texture2D* second = nullptr;
		if (SUCCEEDED(swapChain->GetBuffer(index, IID_PPV_ARGS(&second))) && second) {
			second->AddRef();
			const ULONG secondCount = second->Release();
			getBufferAddsReference = secondCount > firstCount;
			if (getBufferAddsReference) {
				second->Release();
			}
		}

		logger::info(
			"Swap-chain GetBuffer {} ({} release contract)",
			getBufferAddsReference ? "adds a COM reference" : "returns a proxy-owned pointer",
			getBufferAddsReference ? "balanced" : "no direct");
	}

	return texture;
}

void ReleaseAcquiredBackBuffer(ID3D11Texture2D* texture)
{
	if (texture && getBufferAddsReference) {
		texture->Release();
	}
}

ID3D11RenderTargetView* tempRt[2] = {};
ID3D11DepthStencilView* tempSV = nullptr;

struct ScopedRenderTargetReferences
{
	~ScopedRenderTargetReferences()
	{
		SAFE_RELEASE(tempRt[0]);
		SAFE_RELEASE(tempRt[1]);
		SAFE_RELEASE(tempSV);
	}
};

UINT targetIndexCount = 0;
UINT targetStartIndexLocation = 0;
UINT targetBaseVertexLocation = 0;
ComPtr<ID3D11VertexShader> targetVS;

ComPtr<ID3D11PixelShader> targetPS;
ComPtr<ID3D11DepthStencilState> targetDepthStencilState = nullptr;
ComPtr<ID3D11InputLayout> targetInputLayout;

ComPtr<ID3D11Buffer> targetVertexBuffer;
UINT targetVertexBufferStrides;
UINT targetVertexBufferOffsets;

ComPtr<ID3D11ShaderResourceView> DrawIndexedSRV;

bool bHasGetBackBuffer = false;
Hook::D3D* D3DInstance = Hook::D3D::GetSington();

ComPtr<ID3D11Buffer> plane_pIndexBuffer = nullptr;

ID3D11Buffer* gdc_pVertexBuffer = NULL;
ID3D11InputLayout* gdc_pVertexLayout = NULL;
ID3D11Buffer* gdc_pIndexBuffer = NULL;
HMODULE upscalerMod;

using namespace ScopeData;

namespace
{
	HMODULE DetectUpscalerOrFrameGeneration()
	{
		// The original scope-rendering knew only the old Fallout4Upscaler DLL name. LoreOut
		// uses the newer Streamline "Upscaling.dll" plus AAA Frame Generation,
		// so treating this configuration as native TAA is incorrect.
		const char* modules[] = {
			"Fallout4Upscaler.dll",
			"Upscaling.dll",
			"AAAFrameGeneration.dll",
			"sl.interposer.dll"
		};
		for (const auto* module : modules) {
			if (auto handle = GetModuleHandleA(module)) {
				return handle;
			}
		}
		return nullptr;
	}
}

constexpr UINT MAX_SRV_SLOTS = 128;     // D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT
constexpr UINT MAX_SAMPLER_SLOTS = 16;  // D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
constexpr UINT MAX_CB_SLOTS = 14;       // D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
using namespace RE::BSGraphics;

struct SavedState
{
	// IA Stage
	ID3D11InputLayout* pInputLayout;
	ID3D11Buffer* pVertexBuffers[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	UINT VertexStrides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	UINT VertexOffsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	ID3D11Buffer* pIndexBuffer;
	DXGI_FORMAT IndexBufferFormat;
	UINT IndexBufferOffset;
	D3D11_PRIMITIVE_TOPOLOGY PrimitiveTopology;

	// VS Stage
	ID3D11VertexShader* pVS;
	ID3D11Buffer* pVSCBuffers[MAX_CB_SLOTS];
	ID3D11ShaderResourceView* pVSSRVs[MAX_SRV_SLOTS];
	ID3D11SamplerState* pVSSamplers[MAX_SAMPLER_SLOTS];

	// GS Stage. The late ScopeFade replay installs a fill geometry shader, so
	// preserving only VS/PS state would leak that shader into Fallout's next
	// draw after the replay returns. The fill shader also reads the published
	// lens center from b4, so its constant buffers must be preserved too or
	// that binding survives into whatever geometry shader Fallout runs next.
	ID3D11GeometryShader* pGS;

	// PS Stage
	ID3D11PixelShader* pPS;
	ID3D11Buffer* pPSCBuffers[MAX_CB_SLOTS];
	ID3D11ShaderResourceView* pPSSRVs[MAX_SRV_SLOTS];
	ID3D11SamplerState* pPSSamplers[MAX_SAMPLER_SLOTS];

	// RS Stage
	D3D11_VIEWPORT Viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT NumViewports;
	D3D11_RECT ScissorRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT NumScissorRects;
	ID3D11RasterizerState* pRasterizerState;

	// OM Stage
	ID3D11RenderTargetView* pRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	ID3D11DepthStencilView* pDSV;
	ID3D11BlendState* pBlendState;
	FLOAT BlendFactor[4];
	UINT SampleMask;
	ID3D11DepthStencilState* pDepthStencilState;
	UINT StencilRef;
};

namespace Hook
{

	RE::PlayerCharacter* player;
	RE::PlayerCamera* pcam;
	ScopeDataHandler* sdh;

	bool legacyFlag = true;

#define LF(f) (legacyFlag ? (f) : (f) / 1000.0f)

	std::unique_ptr<float[]> LFA(const float arr[], size_t size)
	{
		std::unique_ptr<float[]> arrNew(new float[size]);

		for (size_t i = 0; i < size; i++) {
			arrNew[i] = LF(arr[i]);
		}

		return arrNew;
	}

	const wchar_t* GetWC(const char* c)
	{
		size_t len = strlen(c) + 1;
		size_t converted = 0;
		wchar_t* WStr;
		WStr = (wchar_t*)malloc(len * sizeof(wchar_t));
		mbstowcs_s(&converted, WStr, len, c, _TRUNCATE);
		return WStr;
	}

	HRESULT CreateShaderFromFile(const WCHAR* csoFileNameInOut, const WCHAR* hlslFileName, LPCSTR entryPoint, LPCSTR shaderModel, ID3DBlob** ppBlobOut)
	{
		if (!ppBlobOut) {
			return E_INVALIDARG;
		}
		*ppBlobOut = nullptr;
		HRESULT hr = S_OK;

		if (csoFileNameInOut && D3DReadFileToBlob(csoFileNameInOut, ppBlobOut) == S_OK) {
			return hr;
		}

		if (hlslFileName && std::filesystem::exists(hlslFileName)) {
			DWORD dwShaderFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG

			dwShaderFlags |= D3DCOMPILE_DEBUG;

			dwShaderFlags |= D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
			ID3DBlob* errorBlob = nullptr;
			hr = D3DCompileFromFile(hlslFileName, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entryPoint, shaderModel,
				dwShaderFlags, 0, ppBlobOut, &errorBlob);
			if (FAILED(hr)) {
				if (errorBlob != nullptr) {
					const auto* message = reinterpret_cast<const char*>(errorBlob->GetBufferPointer());
					OutputDebugStringA(message);
					logger::error("Shader compilation failed: {}", message);
				}
				SAFE_RELEASE(errorBlob);
				return hr;
			}
			SAFE_RELEASE(errorBlob);
			return S_OK;
		}

		logger::error("Compiled shader was not found: {}", std::filesystem::path(csoFileNameInOut).string());
		return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
	}

	static XMMATRIX GetProjectionMatrix(float fov)
	{
		if (windowHeight == 0 || windowWidth == 0) {
			const auto* renderWindow = RE::BSGraphics::GetCurrentRendererWindow();
			if (!renderWindow) {
				return XMMatrixIdentity();
			}
			windowWidth = renderWindow->windowWidth;
			windowHeight = renderWindow->windowHeight;
		}

		XMMATRIX projectionMatrix = XMMatrixPerspectiveFovLH(
			fov > XM_PI ? XMConvertToRadians(fov) : fov,
			static_cast<float>(windowWidth) / static_cast<float>(windowHeight),
			0.1f,    // Near clipping plane distance
			1000.0f  // Far clipping plane distance
		);

		return projectionMatrix;
	}

	void SaveState(ID3D11DeviceContext* pContext, SavedState& state)
	{
		// IA Stage
		pContext->IAGetInputLayout(&state.pInputLayout);
		pContext->IAGetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT,
			state.pVertexBuffers, state.VertexStrides, state.VertexOffsets);
		pContext->IAGetIndexBuffer(&state.pIndexBuffer, &state.IndexBufferFormat, &state.IndexBufferOffset);
		pContext->IAGetPrimitiveTopology(&state.PrimitiveTopology);
		// VS Stage
		// Fallout's shipped shaders on this path do not use D3D11 dynamic
		// linkage. Querying the maximum class-instance array is unsafe through
		// common context wrappers: AAAFrameGeneration returned an invalid
		// non-null entry here and Release() crashed on ADS. Preserve the shader
		// object itself, which is the state used by Fallout and the original
		// MagnaScope implementation.
		pContext->VSGetShader(&state.pVS, nullptr, nullptr);
		pContext->VSGetConstantBuffers(0, MAX_CB_SLOTS, state.pVSCBuffers);
		pContext->VSGetShaderResources(0, MAX_SRV_SLOTS, state.pVSSRVs);
		pContext->VSGetSamplers(0, MAX_SAMPLER_SLOTS, state.pVSSamplers);
		// GS Stage
		pContext->GSGetShader(&state.pGS, nullptr, nullptr);
		// PS Stage
		pContext->PSGetShader(&state.pPS, nullptr, nullptr);
		pContext->PSGetConstantBuffers(0, MAX_CB_SLOTS, state.pPSCBuffers);
		pContext->PSGetShaderResources(0, MAX_SRV_SLOTS, state.pPSSRVs);
		pContext->PSGetSamplers(0, MAX_SAMPLER_SLOTS, state.pPSSamplers);
		// RS Stage
		state.NumViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		pContext->RSGetViewports(&state.NumViewports, state.Viewports);
		state.NumScissorRects = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		pContext->RSGetScissorRects(
			&state.NumScissorRects,
			state.ScissorRects);
		pContext->RSGetState(&state.pRasterizerState);
		// OM Stage
		pContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, state.pRTVs, &state.pDSV);
		pContext->OMGetBlendState(&state.pBlendState, state.BlendFactor, &state.SampleMask);
		pContext->OMGetDepthStencilState(&state.pDepthStencilState, &state.StencilRef);
	}

	void RestoreState(ID3D11DeviceContext* pContext, SavedState& state)
	{
		// IA Stage
		pContext->IASetInputLayout(state.pInputLayout);
		pContext->IASetVertexBuffers(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT,
			state.pVertexBuffers, state.VertexStrides, state.VertexOffsets);
		pContext->IASetIndexBuffer(state.pIndexBuffer, state.IndexBufferFormat, state.IndexBufferOffset);
		pContext->IASetPrimitiveTopology(state.PrimitiveTopology);
		// VS Stage
		pContext->VSSetShader(state.pVS, nullptr, 0);
		pContext->VSSetConstantBuffers(0, MAX_CB_SLOTS, state.pVSCBuffers);
		pContext->VSSetShaderResources(0, MAX_SRV_SLOTS, state.pVSSRVs);
		pContext->VSSetSamplers(0, MAX_SAMPLER_SLOTS, state.pVSSamplers);
		// GS Stage
		pContext->GSSetShader(state.pGS, nullptr, 0);
		// PS Stage
		pContext->PSSetShader(state.pPS, nullptr, 0);
		pContext->PSSetConstantBuffers(0, MAX_CB_SLOTS, state.pPSCBuffers);
		pContext->PSSetShaderResources(0, MAX_SRV_SLOTS, state.pPSSRVs);
		pContext->PSSetSamplers(0, MAX_SAMPLER_SLOTS, state.pPSSamplers);
		// RS Stage
		pContext->RSSetViewports(state.NumViewports, state.Viewports);
		pContext->RSSetScissorRects(
			state.NumScissorRects,
			state.ScissorRects);
		pContext->RSSetState(state.pRasterizerState);
		// OM Stage
		pContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, state.pRTVs, state.pDSV);
		pContext->OMSetBlendState(state.pBlendState, state.BlendFactor, state.SampleMask);
		pContext->OMSetDepthStencilState(state.pDepthStencilState, state.StencilRef);
		// 释放临时引用
#define SAFE_RELEASE_ARRAY(arr, count) \
	for (UINT i = 0; i < count; ++i) SAFE_RELEASE(arr[i])
		SAFE_RELEASE(state.pInputLayout);
		SAFE_RELEASE_ARRAY(state.pVertexBuffers, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT);
		SAFE_RELEASE(state.pIndexBuffer);
		SAFE_RELEASE(state.pVS);
		SAFE_RELEASE_ARRAY(state.pVSCBuffers, MAX_CB_SLOTS);
		SAFE_RELEASE_ARRAY(state.pVSSRVs, MAX_SRV_SLOTS);
		SAFE_RELEASE_ARRAY(state.pVSSamplers, MAX_SAMPLER_SLOTS);
		SAFE_RELEASE(state.pGS);
		SAFE_RELEASE(state.pPS);
		SAFE_RELEASE_ARRAY(state.pPSCBuffers, MAX_CB_SLOTS);
		SAFE_RELEASE_ARRAY(state.pPSSRVs, MAX_SRV_SLOTS);
		SAFE_RELEASE_ARRAY(state.pPSSamplers, MAX_SAMPLER_SLOTS);
		SAFE_RELEASE(state.pRasterizerState);
		SAFE_RELEASE_ARRAY(state.pRTVs, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
		SAFE_RELEASE(state.pDSV);
		SAFE_RELEASE(state.pBlendState);
		SAFE_RELEASE(state.pDepthStencilState);
#undef SAFE_RELEASE_ARRAY
	}

	class ScopedContextState
	{
	public:
		explicit ScopedContextState(ID3D11DeviceContext* context) :
			context_(context)
		{
			if (context_) {
				SaveState(context_, state_);
			}
		}

		~ScopedContextState()
		{
			if (context_) {
				RestoreState(context_, state_);
			}
		}

		ScopedContextState(const ScopedContextState&) = delete;
		ScopedContextState& operator=(const ScopedContextState&) = delete;

	private:
		ID3D11DeviceContext* context_;
		SavedState state_{};
	};

	RE::NiPoint3 D3D::WorldToScreen(RE::NiAVObject* cam, RE::NiAVObject* obj, float fov)
	{
		if (!obj) {
			return {};
		}
		return WorldPointToScreen(cam, obj->world.translate, fov);
	}

	RE::NiPoint3 D3D::WorldPointToScreen(
		RE::NiAVObject* cam,
		const RE::NiPoint3& worldPoint,
		float fov)
	{
		// Stages 0 through 2 deliberately do not install MagnaScope's DX11
		// hooks, so OnResize() has not populated these dimensions yet. Resolve
		// them from the renderer window before testing the projection wrapper;
		// otherwise Stage 1 silently returns the origin fallback and never
		// exercises the crash-isolation boundary it is intended to verify.
		if (windowWidth <= 0 || windowHeight <= 0) {
			const auto* renderWindow = RE::BSGraphics::GetCurrentRendererWindow();
			if (renderWindow) {
				windowWidth = static_cast<int>(renderWindow->windowWidth);
				windowHeight = static_cast<int>(renderWindow->windowHeight);
			}
		}
		if (windowWidth <= 0 || windowHeight <= 0) {
			return {
				0.0F,
				0.0F,
				1.0F
			};
		}

		// STS aperture geometry is part of the first-person scene graph. Its
		// coordinates are camera-relative weapon geometry, not main-world
		// coordinates. HUDMenuUtils::WorldPtToScreenPt3 therefore returned
		// (0,0,0) even with the correct function signature.
		//
		// Preserve the original scope-rendering camera contract exactly where it matters:
		// NiMatrix3 stores the first-person Camera rotation in the convention
		// consumed by `camera.world.rotate * (point - camera.translation)`.
		// In that resulting view space, negative Z is forward, X is horizontal,
		// and Y is vertical. The previous inverse-transform/Y-forward
		// replacement produced finite but demonstrably off-screen coordinates
		// for live ScopeFade geometry.
		if (!cam) {
			return {
				static_cast<float>(windowWidth) * 0.5F,
				static_cast<float>(windowHeight) * 0.5F,
				1.0F
			};
		}

		const auto projectScopeAnchor = [&](const RE::NiPoint3& cameraPoint) {
			const float aspect =
				static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
			const float fovRadians = std::clamp(
				fov > XM_PI ? XMConvertToRadians(fov) : fov,
				XMConvertToRadians(1.0F),
				XMConvertToRadians(179.0F));
			const float halfHeight = std::tan(fovRadians * 0.5F);
			const float forward = -cameraPoint.z;
			if (!std::isfinite(forward) || forward <= 0.001F ||
				!std::isfinite(halfHeight) || halfHeight <= 0.0F) {
				return RE::NiPoint3{
					static_cast<float>(windowWidth) * 0.5F,
					static_cast<float>(windowHeight) * 0.5F,
					forward
				};
			}
			const float ndcX =
				-cameraPoint.x / (forward * halfHeight * aspect);
			const float ndcY =
				-cameraPoint.y / (forward * halfHeight);
			return RE::NiPoint3{
				(ndcX + 1.0F) * 0.5F * static_cast<float>(windowWidth),
				(1.0F - ndcY) * 0.5F * static_cast<float>(windowHeight),
				forward
			};
		};

		const RE::NiPoint3 objectWorld = worldPoint;
		const RE::NiPoint3 delta = objectWorld - cam->world.translate;
		const RE::NiPoint3 cameraView = cam->world.rotate * delta;
		const RE::NiPoint3 projected = projectScopeAnchor(cameraView);

		if (player &&
			(player->gunState == RE::GUN_STATE::kSighted ||
				player->gunState == RE::GUN_STATE::kFireSighted)) {
			static std::once_flag loggedProjectionContract;
			std::call_once(loggedProjectionContract, [&] {
				logger::info(
					"First-person aperture projection (original scope-rendering camera contract): cameraView=({:.4f}, {:.4f}, {:.4f}), pixel=({:.2f}, {:.2f}, depth={:.4f}), viewport={}x{}",
					cameraView.x,
					cameraView.y,
					cameraView.z,
					projected.x,
					projected.y,
					projected.z,
					windowWidth,
					windowHeight);
			});
		}

		return projected;
	}

	D3D::ScreenSphereProjection D3D::ProjectWorldSphereToScreen(
		RE::NiAVObject* cam,
		const RE::NiPoint3& worldCenter,
		float worldRadius,
		float fov)
	{
		ScreenSphereProjection result{};
		if (!cam ||
			!std::isfinite(worldCenter.x) ||
			!std::isfinite(worldCenter.y) ||
			!std::isfinite(worldCenter.z) ||
			!std::isfinite(worldRadius) ||
			worldRadius <= 0.001F) {
			return result;
		}

		// Reuse WorldPointToScreen first so the projection viewport is resolved
		// through the same renderer-window fallback used by the center-only
		// verification stages.
		result.center = WorldPointToScreen(cam, worldCenter, fov);
		if (windowWidth <= 0 || windowHeight <= 0 ||
			!std::isfinite(result.center.x) ||
			!std::isfinite(result.center.y) ||
			!std::isfinite(result.center.z) ||
			result.center.z <= 0.001F) {
			return result;
		}

		const float aspect =
			static_cast<float>(windowWidth) / static_cast<float>(windowHeight);
		const float fovRadians = std::clamp(
			fov > XM_PI ? XMConvertToRadians(fov) : fov,
			XMConvertToRadians(1.0F),
			XMConvertToRadians(179.0F));
		const float halfHeight = std::tan(fovRadians * 0.5F);
		if (!std::isfinite(aspect) || aspect <= 0.0F ||
			!std::isfinite(halfHeight) || halfHeight <= 0.0F) {
			return result;
		}

		// worldBound.fRadius is already scaled into world units. Transform the
		// bound center once through the verified original scope-rendering camera contract,
		// then offset it along camera X and Y. A sphere has the same radius in
		// every orientation, so no scene-graph basis reconstruction is needed.
		const RE::NiPoint3 cameraCenter =
			cam->world.rotate * (worldCenter - cam->world.translate);
		const auto projectCameraPoint = [&](const RE::NiPoint3& cameraPoint) {
			const float forward = -cameraPoint.z;
			if (!std::isfinite(forward) || forward <= 0.001F) {
				return RE::NiPoint3{
					result.center.x,
					result.center.y,
					forward
				};
			}

			const float ndcX =
				-cameraPoint.x / (forward * halfHeight * aspect);
			const float ndcY =
				-cameraPoint.y / (forward * halfHeight);
			return RE::NiPoint3{
				(ndcX + 1.0F) * 0.5F * static_cast<float>(windowWidth),
				(1.0F - ndcY) * 0.5F * static_cast<float>(windowHeight),
				forward
			};
		};

		RE::NiPoint3 horizontalEdge = cameraCenter;
		horizontalEdge.x += worldRadius;
		RE::NiPoint3 verticalEdge = cameraCenter;
		verticalEdge.y += worldRadius;
		const RE::NiPoint3 projectedHorizontal =
			projectCameraPoint(horizontalEdge);
		const RE::NiPoint3 projectedVertical =
			projectCameraPoint(verticalEdge);

		result.radiusX = std::abs(projectedHorizontal.x - result.center.x);
		result.radiusY = std::abs(projectedVertical.y - result.center.y);
		result.valid =
			std::isfinite(result.radiusX) &&
			std::isfinite(result.radiusY) &&
			result.radiusX > 1.0F &&
			result.radiusY > 1.0F &&
			result.radiusX < static_cast<float>(windowWidth) &&
			result.radiusY < static_cast<float>(windowHeight);
		return result;
	}

	bool D3D::InitEffect()
	{
		ComPtr<ID3DBlob> blob;
		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\ScopeEffect_PS.cso", L"src\\HLSL\\ScopeEffect_PS.hlsl", "main", "ps_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_pPixelShader.GetAddressOf()));

		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\ScopeEffect_PS_Output.cso", L"src\\HLSL\\ScopeEffect_PS_Output.hlsl", "main", "ps_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_outPutPixelShader.GetAddressOf()));

		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\ScopeEffect_VS.cso", L"src\\HLSL\\ScopeEffect_VS.hlsl", "main", "vs_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_pVertexShader.GetAddressOf()));

		HR(g_Device->CreateInputLayout(gdc_layout, ARRAYSIZE(gdc_layout), blob->GetBufferPointer(), blob->GetBufferSize(), &gdc_pVertexLayout));

		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\ScopeEffect_VS_Output.cso", L"src\\HLSL\\ScopeEffect_VS_Output.hlsl", "main", "vs_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_outPutVertexShader.GetAddressOf()));

		return m_pPixelShader.Get() && m_outPutPixelShader.Get() && m_pVertexShader.Get() && m_outPutVertexShader.Get();
	}

	bool D3D::InitLegacyEffect()
	{
		ComPtr<ID3DBlob> blob;

		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\AutoSTS_PS.cso", L"src\\HLSL\\AutoSTS_PS.hlsl", "main", "ps_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_pPixelShader_AutoSTS.GetAddressOf()));

		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\ScopeEffect_PS_Legacy.cso", L"src\\HLSL\\ScopeEffect_PS_Legacy.hlsl", "main", "ps_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_pPixelShader_Legacy.GetAddressOf()));

		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\ScopeEffect_PS_Output_Legacy.cso", L"src\\HLSL\\ScopeEffect_PS_Output_Legacy.hlsl", "main", "ps_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_outPutPixelShader_Legacy.GetAddressOf()));

		if (FAILED(CreateShaderFromFile(L"Data\\Shaders\\MagnaScope\\ScopeEffect_VS_Legacy.cso", L"src\\HLSL\\ScopeEffect_VS_Legacy.hlsl", "main", "vs_5_0", blob.ReleaseAndGetAddressOf())) || !blob.Get())
			return false;
		HR(g_Device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, m_pVertexShader_Legacy.GetAddressOf()));
		return m_pPixelShader_AutoSTS.Get() && m_pPixelShader_Legacy.Get() && m_outPutPixelShader_Legacy.Get() && m_pVertexShader_Legacy.Get();
	}

	bool D3D::InitGeometryProbeEffect()
	{
		if (FAILED(CreateShaderFromFile(
				L"Data\\Shaders\\MagnaScope\\ScopeGeometryProbe_PS.cso",
				L"src\\HLSL\\ScopeGeometryProbe_PS.hlsl",
				"main",
				"ps_5_0",
				mScopeFadeProbePixelBytecode.ReleaseAndGetAddressOf())) ||
			!mScopeFadeProbePixelBytecode.Get()) {
			logger::error(
				"Stage 4d geometry probe shader could not be loaded; "
				"the ScopeFade draw will remain untouched");
			return false;
		}

		if (MagnaScope::GetSettings().AllowsGeometryMagnification()) {
			if (FAILED(CreateShaderFromFile(
					L"Data\\Shaders\\MagnaScope\\ScopeGeometryMagnify_PS.cso",
					L"src\\HLSL\\ScopeGeometryMagnify_PS.hlsl",
					"main",
					"ps_5_0",
					mScopeFadeMagnifyPixelBytecode.ReleaseAndGetAddressOf())) ||
				!mScopeFadeMagnifyPixelBytecode.Get()) {
				logger::error(
					"Stage 4e.2 geometry magnification shader could not be "
					"loaded; the ScopeFade draw will remain untouched");
				return false;
			}

			if (FAILED(CreateShaderFromFile(
					L"Data\\Shaders\\MagnaScope\\ReticleLayer_PS.cso",
					L"src\\HLSL\\ReticleLayer_PS.hlsl",
					"main",
					"ps_5_0",
					mReticleLayerPixelBytecode.ReleaseAndGetAddressOf())) ||
				!mReticleLayerPixelBytecode.Get()) {
				logger::error(
					"Independent STS reticle layer shader could not be "
					"loaded; automatic magnification will fail closed");
				return false;
			}
		}

		if (FAILED(CreateShaderFromFile(
				L"Data\\Shaders\\MagnaScope\\ScopeGeometryFill_GS.cso",
				L"src\\HLSL\\ScopeGeometryFill_GS.hlsl",
				"main",
				"gs_5_0",
				mScopeFadeFillGeometryBytecode.ReleaseAndGetAddressOf())) ||
			!mScopeFadeFillGeometryBytecode.Get()) {
			logger::error(
				"Stage 4d geometry fill shader could not be loaded; "
				"the ScopeFade draw will remain untouched");
			return false;
		}

		// Create an initial set on Fallout's advertised renderer device. The
		// exact DrawIndexed path verifies and, when necessary, rebuilds these
		// children on the device returned by that draw context. This is the
		// D3D11 ownership contract used by the original scope-rendering interception path
		// and remains correct through ENB and upscaler proxy interfaces.
		if (!EnsureGeometryProbeDeviceResources(g_Device.Get())) {
			logger::error(
				"ScopeFade device resources could not be initialized; "
				"all matching draws will remain untouched");
			return false;
		}

		logger::info(
			"ScopeFade geometry bytecode initialized for {}; only an exact "
			"published ScopeFade buffer match may use them",
			MagnaScope::GetSettings().AllowsGeometryMagnification() ?
				"Stage 4e.2 profile-driven scene magnification" :
				"Stage 4d cyan probing");
		return true;
	}

	bool D3D::EnsureGeometryProbeDeviceResources(ID3D11Device* device)
	{
		if (!device ||
			!mScopeFadeProbePixelBytecode.Get() ||
			!mScopeFadeFillGeometryBytecode.Get() ||
			(MagnaScope::GetSettings().AllowsGeometryMagnification() &&
				(!mScopeFadeMagnifyPixelBytecode.Get() ||
					!mReticleLayerPixelBytecode.Get()))) {
			return false;
		}

		const bool sameDevice =
			mScopeFadeResourceDevice.Get() &&
			HaveSameCOMIdentity(mScopeFadeResourceDevice.Get(), device);
		const bool complete =
			m_pPixelShader_STSGeometryProbe.Get() &&
			m_pGeometryShader_STSGeometryFill.Get() &&
			(!MagnaScope::GetSettings().AllowsGeometryMagnification() ||
				(m_pPixelShader_STSGeometryMagnify.Get() &&
					m_pPixelShader_STSReticleLayer.Get() &&
					mScopeFadeResolutionBuffer.Get() &&
					mScopeFadeSampler.Get() &&
					BSScopeFadeReplaceRGB.Get()));
		if (sameDevice && complete) {
			return true;
		}

		std::scoped_lock reticleLayerLock(
			mAutomaticSTSReticleLayerMutex);
		const bool replacingDevice = mScopeFadeResourceDevice.Get() != nullptr;
		mScopeFadeSceneSRV.Reset();
		mScopeFadeSceneTexture.Reset();
		mAutomaticSTSReticleLayerWhiteSRV.Reset();
		mAutomaticSTSReticleLayerWhiteRTV.Reset();
		mAutomaticSTSReticleLayerWhiteTexture.Reset();
		mAutomaticSTSReticleLayerSRV.Reset();
		mAutomaticSTSReticleLayerRTV.Reset();
		mAutomaticSTSReticleLayerTexture.Reset();
		mAutomaticSTSReticleLayerCompositeBlend.Reset();
		mAutomaticSTSReticleSuppressionSourceBlend.Reset();
		mAutomaticSTSReticleColorSuppressionBlend.Reset();
		mAutomaticSTSReticleSuppressionSourceWasNull = false;
		mAutomaticSTSReticleLayerReadOnlyDepthState.Reset();
		mAutomaticSTSReticleLayerCaptureGeneration = 0U;
		mAutomaticSTSReticleLayerGeneration = 0U;
		mAutomaticSTSReticleLayerResourceGeneration = 0U;
		mAutomaticSTSReticleLayerReady = false;
		mScopeFadeSampler.Reset();
		mScopeFadeResolutionBuffer.Reset();
		BSScopeFadeReplaceRGB.Reset();
		m_pGeometryShader_STSGeometryFill.Reset();
		m_pPixelShader_STSGeometryMagnify.Reset();
		m_pPixelShader_STSReticleLayer.Reset();
		m_pPixelShader_STSGeometryProbe.Reset();
		mScopeFadeResourceDevice.Reset();

		const HRESULT probeResult = device->CreatePixelShader(
			mScopeFadeProbePixelBytecode->GetBufferPointer(),
			mScopeFadeProbePixelBytecode->GetBufferSize(),
			nullptr,
			m_pPixelShader_STSGeometryProbe.ReleaseAndGetAddressOf());
		if (FAILED(probeResult) || !m_pPixelShader_STSGeometryProbe.Get()) {
			logger::error(
				"Stage 4d geometry probe pixel shader creation failed on "
				"the draw-context device: 0x{:08X}",
				static_cast<std::uint32_t>(probeResult));
			return false;
		}

		const HRESULT geometryResult = device->CreateGeometryShader(
			mScopeFadeFillGeometryBytecode->GetBufferPointer(),
			mScopeFadeFillGeometryBytecode->GetBufferSize(),
			nullptr,
			m_pGeometryShader_STSGeometryFill.ReleaseAndGetAddressOf());
		if (FAILED(geometryResult) ||
			!m_pGeometryShader_STSGeometryFill.Get()) {
			logger::error(
				"Stage 4d geometry fill shader creation failed on the "
				"draw-context device: 0x{:08X}",
				static_cast<std::uint32_t>(geometryResult));
			m_pPixelShader_STSGeometryProbe.Reset();
			return false;
		}

		if (MagnaScope::GetSettings().AllowsGeometryMagnification()) {
			const HRESULT magnifyResult = device->CreatePixelShader(
				mScopeFadeMagnifyPixelBytecode->GetBufferPointer(),
				mScopeFadeMagnifyPixelBytecode->GetBufferSize(),
				nullptr,
				m_pPixelShader_STSGeometryMagnify.ReleaseAndGetAddressOf());
			if (FAILED(magnifyResult) ||
				!m_pPixelShader_STSGeometryMagnify.Get()) {
				logger::error(
					"Stage 4e.2 magnification pixel shader creation failed "
					"on the draw-context device: 0x{:08X}",
					static_cast<std::uint32_t>(magnifyResult));
				m_pGeometryShader_STSGeometryFill.Reset();
				m_pPixelShader_STSGeometryProbe.Reset();
				return false;
			}

			const HRESULT reticleLayerResult = device->CreatePixelShader(
				mReticleLayerPixelBytecode->GetBufferPointer(),
				mReticleLayerPixelBytecode->GetBufferSize(),
				nullptr,
				m_pPixelShader_STSReticleLayer
					.ReleaseAndGetAddressOf());
			if (FAILED(reticleLayerResult) ||
				!m_pPixelShader_STSReticleLayer.Get()) {
				logger::error(
					"Independent STS reticle layer pixel shader creation "
					"failed: 0x{:08X}",
					static_cast<std::uint32_t>(reticleLayerResult));
				m_pPixelShader_STSGeometryMagnify.Reset();
				m_pGeometryShader_STSGeometryFill.Reset();
				m_pPixelShader_STSGeometryProbe.Reset();
				return false;
			}

			D3D11_BUFFER_DESC resolutionDescription{};
			resolutionDescription.ByteWidth =
				static_cast<UINT>(sizeof(ConstBufferData));
			resolutionDescription.Usage = D3D11_USAGE_DYNAMIC;
			resolutionDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			resolutionDescription.CPUAccessFlags =
				D3D11_CPU_ACCESS_WRITE;
			const HRESULT resolutionResult = device->CreateBuffer(
				&resolutionDescription,
				nullptr,
				mScopeFadeResolutionBuffer.ReleaseAndGetAddressOf());
			if (FAILED(resolutionResult) ||
				!mScopeFadeResolutionBuffer.Get()) {
				logger::error(
					"Stage 4e.2 resolution buffer creation failed on the "
					"draw-context device: 0x{:08X}",
					static_cast<std::uint32_t>(resolutionResult));
				m_pPixelShader_STSGeometryMagnify.Reset();
				m_pGeometryShader_STSGeometryFill.Reset();
				m_pPixelShader_STSGeometryProbe.Reset();
				return false;
			}

			D3D11_SAMPLER_DESC samplerDescription{};
			samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDescription.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDescription.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDescription.MinLOD = 0.0F;
			samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
			const HRESULT samplerResult = device->CreateSamplerState(
				&samplerDescription,
				mScopeFadeSampler.ReleaseAndGetAddressOf());
			if (FAILED(samplerResult) || !mScopeFadeSampler.Get()) {
				logger::error(
					"Stage 4e.2 sampler creation failed on the "
					"draw-context device: 0x{:08X}",
					static_cast<std::uint32_t>(samplerResult));
				mScopeFadeResolutionBuffer.Reset();
				m_pPixelShader_STSGeometryMagnify.Reset();
				m_pGeometryShader_STSGeometryFill.Reset();
				m_pPixelShader_STSGeometryProbe.Reset();
				return false;
			}

			D3D11_BLEND_DESC replacementBlendDescription{};
			replacementBlendDescription.AlphaToCoverageEnable = false;
			replacementBlendDescription.IndependentBlendEnable = false;
			auto& replacementTarget =
				replacementBlendDescription.RenderTarget[0];
			replacementTarget.BlendEnable = false;
			replacementTarget.RenderTargetWriteMask =
				D3D11_COLOR_WRITE_ENABLE_RED |
				D3D11_COLOR_WRITE_ENABLE_GREEN |
				D3D11_COLOR_WRITE_ENABLE_BLUE;
			const HRESULT replacementBlendResult =
				device->CreateBlendState(
					&replacementBlendDescription,
					BSScopeFadeReplaceRGB.ReleaseAndGetAddressOf());
			if (FAILED(replacementBlendResult) ||
				!BSScopeFadeReplaceRGB.Get()) {
				logger::error(
					"Stage 4g ScopeFade RGB replacement blend creation "
					"failed: 0x{:08X}",
					static_cast<std::uint32_t>(replacementBlendResult));
				mScopeFadeSampler.Reset();
				mScopeFadeResolutionBuffer.Reset();
				m_pPixelShader_STSGeometryMagnify.Reset();
				m_pGeometryShader_STSGeometryFill.Reset();
				m_pPixelShader_STSGeometryProbe.Reset();
				return false;
			}
		}

		mScopeFadeResourceDevice = device;
		mScopeFadeResourceGeneration.fetch_add(
			1U,
			std::memory_order_acq_rel);
		if (replacingDevice) {
			logger::info(
				"Stage 4e.2 rebound ScopeFade shaders and source resources "
				"to the exact DrawIndexed context device");
		}
		return true;
	}

	bool D3D::PrepareScopeFadeSceneSource(
		ID3D11DeviceContext* context,
		ID3D11RenderTargetView* renderTarget,
		ID3D11ShaderResourceView* preferredWorldSource)
	{
		if (!context || !renderTarget) {
			return false;
		}

		ComPtr<ID3D11Device> contextDevice;
		context->GetDevice(contextDevice.GetAddressOf());
		if (!contextDevice.Get() ||
			!EnsureGeometryProbeDeviceResources(contextDevice.Get())) {
			return false;
		}

		ComPtr<ID3D11Resource> sourceResource;
		if (preferredWorldSource) {
			preferredWorldSource->GetResource(sourceResource.GetAddressOf());
		} else {
			renderTarget->GetResource(sourceResource.GetAddressOf());
		}
		ComPtr<ID3D11Texture2D> sourceTexture;
		if (!sourceResource.Get() ||
			FAILED(sourceResource.As(&sourceTexture)) ||
			!sourceTexture.Get()) {
			static std::once_flag loggedNonTextureTarget;
			std::call_once(loggedNonTextureTarget, [] {
				logger::warn(
					"Stage 4e.2 ScopeFade color source is not a "
					"Texture2D; the authored draw is left untouched");
			});
			return false;
		}

		D3D11_TEXTURE2D_DESC sourceDescription{};
		sourceTexture->GetDesc(&sourceDescription);
		ComPtr<ID3D11Resource> targetResource;
		renderTarget->GetResource(targetResource.GetAddressOf());
		ComPtr<ID3D11Texture2D> targetTexture;
		if (!targetResource.Get() ||
			FAILED(targetResource.As(&targetTexture)) ||
			!targetTexture.Get()) {
			return false;
		}
		D3D11_TEXTURE2D_DESC targetTextureDescription{};
		targetTexture->GetDesc(&targetTextureDescription);
		D3D11_RENDER_TARGET_VIEW_DESC targetDescription{};
		renderTarget->GetDesc(&targetDescription);
		D3D11_SHADER_RESOURCE_VIEW_DESC preferredViewDescription{};
		if (preferredWorldSource) {
			preferredWorldSource->GetDesc(&preferredViewDescription);
			// D3D11 forbids sampling a resource while the same controlling
			// resource is bound for output. The normal late path uses
			// mCurRTTexture as a private coherent copy, but fail closed if an
			// ENB/upscaler proxy ever aliases that SRV with the displayed RTV.
			if (HaveSameCOMIdentity(
					sourceResource.Get(),
					targetResource.Get())) {
				static std::once_flag loggedAliasedScopeSource;
				std::call_once(loggedAliasedScopeSource, [] {
					logger::warn(
						"Automatic STS late source aliases its output target; "
						"the authored STS lens is retained");
				});
				return false;
			}
		}
		// OMGetRenderTargets returned this texture from the exact context
		// whose draw is being replaced. That active binding is the
		// authoritative compatibility contract. Comparing GetDevice()
		// interface identities here is both redundant and incorrect under
		// ENB/upscaler proxies, which may expose distinct COM interfaces for
		// the same native device.

		// The first rollout supports the color target Fallout 4 uses in the
		// verified OG ScopeFade path. Refuse unverified MSAA, arrays, mips, and
		// unknown view formats instead of manufacturing a mismatched resource
		// or invoking ResolveSubresource with guessed semantics.
		const bool supportedTarget =
			sourceDescription.Width > 0 &&
			sourceDescription.Height > 0 &&
			sourceDescription.MipLevels == 1 &&
			sourceDescription.ArraySize == 1 &&
			sourceDescription.SampleDesc.Count == 1 &&
			sourceDescription.Width == targetTextureDescription.Width &&
			sourceDescription.Height == targetTextureDescription.Height &&
			sourceDescription.SampleDesc.Count ==
				targetTextureDescription.SampleDesc.Count &&
			targetDescription.ViewDimension ==
				D3D11_RTV_DIMENSION_TEXTURE2D &&
			targetDescription.Format != DXGI_FORMAT_UNKNOWN &&
			(!preferredWorldSource ||
				(preferredViewDescription.ViewDimension ==
					 D3D11_SRV_DIMENSION_TEXTURE2D &&
				 preferredViewDescription.Format != DXGI_FORMAT_UNKNOWN &&
				 preferredViewDescription.Texture2D.MostDetailedMip == 0 &&
				 preferredViewDescription.Texture2D.MipLevels == 1));
		if (!supportedTarget) {
			static std::once_flag loggedUnsupportedTarget;
			std::call_once(loggedUnsupportedTarget, [&] {
				logger::warn(
					"Stage 4e.2 rejected ScopeFade color target: "
					"{}x{}, format={}, RTV format={}, mips={}, array={}, "
					"samples={}, view={}; the authored draw is untouched",
					sourceDescription.Width,
					sourceDescription.Height,
					static_cast<int>(sourceDescription.Format),
					static_cast<int>(targetDescription.Format),
					sourceDescription.MipLevels,
					sourceDescription.ArraySize,
					sourceDescription.SampleDesc.Count,
					static_cast<int>(targetDescription.ViewDimension));
			});
			return false;
		}

		bool recreate =
			!preferredWorldSource &&
			(!mScopeFadeSceneTexture.Get() || !mScopeFadeSceneSRV.Get());
		if (!recreate) {
			if (!preferredWorldSource) {
				D3D11_TEXTURE2D_DESC existingDescription{};
				mScopeFadeSceneTexture->GetDesc(&existingDescription);
				recreate =
					existingDescription.Width != sourceDescription.Width ||
					existingDescription.Height != sourceDescription.Height ||
					existingDescription.Format != sourceDescription.Format ||
					existingDescription.SampleDesc.Count !=
						sourceDescription.SampleDesc.Count ||
					existingDescription.SampleDesc.Quality !=
						sourceDescription.SampleDesc.Quality;
			}
		}

		if (recreate) {
			mScopeFadeSceneSRV.Reset();
			mScopeFadeSceneTexture.Reset();

			D3D11_TEXTURE2D_DESC copyDescription = sourceDescription;
			copyDescription.Usage = D3D11_USAGE_DEFAULT;
			copyDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			copyDescription.CPUAccessFlags = 0;
			copyDescription.MiscFlags = 0;
			const HRESULT textureResult = contextDevice->CreateTexture2D(
				&copyDescription,
				nullptr,
				mScopeFadeSceneTexture.ReleaseAndGetAddressOf());
			if (FAILED(textureResult) || !mScopeFadeSceneTexture.Get()) {
				logger::error(
					"Stage 4e.2 could not create the private ScopeFade "
					"scene copy: 0x{:08X}",
					static_cast<std::uint32_t>(textureResult));
				return false;
			}

			D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
			viewDescription.Format = targetDescription.Format;
			viewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			viewDescription.Texture2D.MostDetailedMip = 0;
			viewDescription.Texture2D.MipLevels = 1;
			const HRESULT viewResult =
				contextDevice->CreateShaderResourceView(
					mScopeFadeSceneTexture.Get(),
					&viewDescription,
					mScopeFadeSceneSRV.ReleaseAndGetAddressOf());
			if (FAILED(viewResult) || !mScopeFadeSceneSRV.Get()) {
				logger::error(
					"Stage 4e.2 could not create the ScopeFade scene SRV "
					"for format {}: 0x{:08X}",
					static_cast<int>(targetDescription.Format),
					static_cast<std::uint32_t>(viewResult));
				mScopeFadeSceneTexture.Reset();
				return false;
			}

			logger::info(
				"Stage 4e.2 ScopeFade scene source created: {}x{}, "
				"resource format={}, view format={}",
				sourceDescription.Width,
				sourceDescription.Height,
				static_cast<int>(sourceDescription.Format),
				static_cast<int>(targetDescription.Format));
		}

		if (!preferredWorldSource) {
			context->CopyResource(
				mScopeFadeSceneTexture.Get(),
				sourceTexture.Get());
		}

		const auto projection = GetLensProjectionSnapshot();
		const float renderWidth =
			static_cast<float>(sourceDescription.Width);
		const float renderHeight =
			static_cast<float>(sourceDescription.Height);
		const bool validProjection =
			projection.trackingReady &&
			projection.automaticSTS &&
			std::isfinite(projection.centerX) &&
			std::isfinite(projection.centerY) &&
			std::isfinite(projection.aimCenterX) &&
			std::isfinite(projection.aimCenterY) &&
			std::isfinite(projection.radiusX) &&
			std::isfinite(projection.radiusY) &&
			std::isfinite(projection.sourceWidth) &&
			std::isfinite(projection.sourceHeight) &&
			projection.sourceWidth > 0.0F &&
			projection.sourceHeight > 0.0F &&
			projection.radiusX > 0.0F &&
			projection.radiusY > 0.0F;
		const float projectionScaleX =
			validProjection ?
				renderWidth / projection.sourceWidth :
				1.0F;
		const float projectionScaleY =
			validProjection ?
				renderHeight / projection.sourceHeight :
				1.0F;

		ConstBufferData resolution{};
		resolution.width = renderWidth;
		resolution.height = renderHeight;
		resolution.scopeFadeMagnification = std::clamp(
			scopeFadeMagnification.load(std::memory_order_acquire),
			1.0F,
			15.0F);
		resolution.activationProgress =
			validProjection ?
				std::clamp(
					projection.activationProgress,
					0.0F,
					1.0F) :
				0.0F;
		if (validProjection) {
			resolution.lensRadiusX =
				projection.radiusX * projectionScaleX;
			resolution.lensRadiusY =
				projection.radiusY * projectionScaleY;
			resolution.lensCenterX =
				projection.centerX * projectionScaleX;
			resolution.lensCenterY =
				projection.centerY * projectionScaleY;
			resolution.aimCenterX =
				projection.aimCenterX * projectionScaleX;
			resolution.aimCenterY =
				projection.aimCenterY * projectionScaleY;
		}
		// Bounded so a malformed profile can shift the sight picture off the
		// aperture or collapse it, but never place it outside the housing
		// entirely or shrink it to a pinhole.
		resolution.lensOffsetX = std::clamp(
			scopeLensOffsetX.load(std::memory_order_acquire),
			-1.0F,
			1.0F);
		resolution.lensOffsetY = std::clamp(
			scopeLensOffsetY.load(std::memory_order_acquire),
			-1.0F,
			1.0F);
		resolution.lensScale = std::clamp(
			scopeLensScale.load(std::memory_order_acquire),
			0.25F,
			2.0F);
		resolution.breathPhase =
			scopeBreathPhase.load(std::memory_order_acquire);
		resolution.breathSway = std::clamp(
			scopeBreathSway.load(std::memory_order_acquire),
			0.0F,
			1.0F);
		resolution.breathDrift = std::clamp(
			scopeBreathDrift.load(std::memory_order_acquire),
			0.0F,
			1.0F);
		resolution.breathFigure = std::clamp(
			scopeBreathFigure.load(std::memory_order_acquire),
			0.0F,
			1.0F);
		resolution.breathHold = std::clamp(
			scopeBreathHold.load(std::memory_order_acquire),
			0.0F,
			1.0F);
		resolution.breathPupilFollow = std::clamp(
			scopeBreathPupilFollow.load(std::memory_order_acquire),
			0.0F,
			2.0F);
		resolution.imageDenoise = std::clamp(
			scopeImageDenoise.load(std::memory_order_acquire),
			0.0F,
			1.0F);
		resolution.imageSharpen = std::clamp(
			scopeImageSharpen.load(std::memory_order_acquire),
			0.0F,
			1.0F);
		resolution.fishEyeStrength = std::clamp(
			scopeFishEyeStrength.load(std::memory_order_acquire),
			0.0F,
			2.0F);
		resolution.fishEyePower = std::clamp(
			scopeFishEyePower.load(std::memory_order_acquire),
			0.5F,
			6.0F);
		resolution.edgeRefractionStrength = std::clamp(
			scopeEdgeRefractionStrength.load(std::memory_order_acquire),
			0.0F,
			0.25F);
		resolution.edgeRefractionWidth = std::clamp(
			scopeEdgeRefractionWidth.load(std::memory_order_acquire),
			0.02F,
			0.5F);
		resolution.edgeChromaticAberration = std::clamp(
			scopeEdgeChromaticAberration.load(std::memory_order_acquire),
			0.0F,
			2.0F);
		resolution.sceneParallaxStrength = std::clamp(
			scopeSceneParallaxStrength.load(std::memory_order_acquire),
			0.0F,
			2.0F);
		resolution.opticalLagStrength = std::clamp(
			scopeOpticalLagStrength.load(std::memory_order_acquire),
			0.0F,
			4.0F);
		resolution.reticleShadowStrength = std::clamp(
			scopeReticleShadowStrength.load(std::memory_order_acquire),
			0.0F,
			1.0F);
		resolution.reticleParallaxStrength = std::clamp(
			scopeReticleParallaxStrength.load(std::memory_order_acquire),
			0.0F,
			4.0F);
		if (validProjection && projection.physicalEyeBoxReady) {
			// Convert the game-thread projection into this render target's
			// pixel space. The basis carries ScopeFade roll and perspective,
			// so the shader never assumes a screen-aligned optic.
			resolution.eyeOffsetX = projection.eyeOffsetX;
			resolution.eyeOffsetY = projection.eyeOffsetY;
			resolution.eyeReliefDelta = std::clamp(
				projection.eyeReliefDelta,
				-0.25F,
				0.25F);
			resolution.physicalEyeBoxValid = std::clamp(
				projection.physicalEyeBoxBlend,
				0.0F,
				1.0F);
			resolution.lensBasisXX =
				projection.lensBasisXX * projectionScaleX;
			resolution.lensBasisXY =
				projection.lensBasisXY * projectionScaleY;
			resolution.lensBasisZX =
				projection.lensBasisZX * projectionScaleX;
			resolution.lensBasisZY =
				projection.lensBasisZY * projectionScaleY;

			// Preserve the manually authored reticle-to-ScopeFade relation in
			// lens-local units. Fixed screen pixels would become incorrect as
			// the weapon rolls or an inertia mod moves the optic after this
			// CPU snapshot. The exact DrawIndexed shader reconstructs pixels
			// from these coordinates and its current transformed vertices.
			const float aimDeltaX =
				(projection.aimCenterX - projection.centerX) *
				projectionScaleX;
			const float aimDeltaY =
				(projection.aimCenterY - projection.centerY) *
				projectionScaleY;
			const float basisDeterminant =
				resolution.lensBasisXX * resolution.lensBasisZY -
				resolution.lensBasisXY * resolution.lensBasisZX;
			if (std::isfinite(basisDeterminant) &&
				std::abs(basisDeterminant) > 0.0001F) {
				resolution.aimOffsetX =
					(aimDeltaX * resolution.lensBasisZY -
					 aimDeltaY * resolution.lensBasisZX) /
					basisDeterminant;
				resolution.aimOffsetY =
					(-aimDeltaX * resolution.lensBasisXY +
					 aimDeltaY * resolution.lensBasisXX) /
					basisDeterminant;
				resolution.aimOffsetValid =
					std::isfinite(resolution.aimOffsetX) &&
							std::isfinite(resolution.aimOffsetY) ?
						1.0F :
						0.0F;
			}
		}
		resolution.eyeBoxRadius = std::clamp(
			scopeEyeBoxRadius.load(std::memory_order_acquire),
			0.01F,
			20.0F);
		resolution.vignetteReach = std::clamp(
			scopeVignetteReach.load(std::memory_order_acquire),
			1.01F,
			20.0F);
		resolution.vignetteSharpness = std::clamp(
			scopeVignetteSharpness.load(std::memory_order_acquire),
			0.1F,
			20.0F);
		resolution.eyeBoxMaxTravel = std::clamp(
			scopeEyeBoxMaxTravel.load(std::memory_order_acquire),
			0.0F,
			4.0F);
		resolution.reticleMagnification = std::clamp(
			scopeReticleMagnification.load(
				std::memory_order_acquire),
			0.25F,
			8.0F);
		resolution.reticleSize = std::clamp(
			scopeReticleSize.load(std::memory_order_acquire),
			0.01F,
			128.0F);
		resolution.reticleOffsetX = std::clamp(
			scopeReticleOffsetX.load(std::memory_order_acquire),
			-1000.0F,
			1000.0F);
		resolution.reticleOffsetY = std::clamp(
			scopeReticleOffsetY.load(std::memory_order_acquire),
			-1000.0F,
			1000.0F);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		const HRESULT mapResult = context->Map(
			mScopeFadeResolutionBuffer.Get(),
			0,
			D3D11_MAP_WRITE_DISCARD,
			0,
			&mapped);
		if (FAILED(mapResult) || !mapped.pData) {
			logger::error(
				"Stage 4e.2 could not map the resolution buffer: "
				"0x{:08X}",
				static_cast<std::uint32_t>(mapResult));
			return false;
		}
		std::memcpy(mapped.pData, &resolution, sizeof(resolution));
		context->Unmap(mScopeFadeResolutionBuffer.Get(), 0);
		return true;
	}

	bool D3D::CaptureAutomaticSTSScopeFadeReplay(
		ID3D11DeviceContext* context,
		UINT indexCount,
		UINT startIndexLocation,
		INT baseVertexLocation,
		ID3D11Buffer* vertexBuffer,
		UINT vertexStride,
		UINT vertexOffset,
		ID3D11Buffer* indexBuffer,
		DXGI_FORMAT indexFormat,
		UINT indexOffset)
	{
		if (!context || !vertexBuffer || !indexBuffer || indexCount == 0 ||
			vertexStride == 0 ||
			(indexFormat != DXGI_FORMAT_R16_UINT &&
				indexFormat != DXGI_FORMAT_R32_UINT)) {
			return false;
		}
		if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
			!g_Context.Get() ||
			!HaveSameCOMIdentity(context, g_Context.Get())) {
			// CopyResource into the replay's private transform buffers must
			// execute before the late callback. A deferred or foreign context
			// does not provide that ordering contract.
			return false;
		}

		// The exact ScopeFade classifier already holds
		// mScopeFadeGeometryMutex while it validates the published buffer
		// identity and captures this replay packet. Do not lock it again here:
		// std::mutex is non-recursive, and a nested lock would deadlock the
		// render thread on the first matching scope draw.
		auto& replay = mAutomaticSTSScopeFadeReplay;
		replay.ready = false;

		ComPtr<ID3D11VertexShader> vertexShader;
		// ScopeFade's material shader has no dynamic-linkage class instances.
		// Capture only the shader object. Supplying the maximum interface array
		// to a wrapped context caused AAAFrameGeneration's proxy to return an
		// invalid entry whose COM Release dispatch crashed at address 0x10.
		context->VSGetShader(vertexShader.GetAddressOf(), nullptr, nullptr);
		ComPtr<ID3D11InputLayout> inputLayout;
		context->IAGetInputLayout(inputLayout.GetAddressOf());
		D3D11_PRIMITIVE_TOPOLOGY topology =
			D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		context->IAGetPrimitiveTopology(&topology);
		if (!vertexShader.Get() || !inputLayout.Get() ||
			topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST) {
			return false;
		}

		ComPtr<ID3D11Device> device;
		context->GetDevice(device.GetAddressOf());
		if (!device.Get() || !g_Device.Get() ||
			!HaveSameCOMIdentity(device.Get(), g_Device.Get()) ||
			!EnsureGeometryProbeDeviceResources(device.Get())) {
			return false;
		}

		// Fallout's first-person material vertex shader reads these three
		// transform buffers. Copy their contents at the exact ScopeFade draw
		// instead of retaining the live buffers, because the engine reuses and
		// overwrites them before the late TAA or Present callback.
		const auto resourceGeneration =
			mScopeFadeResourceGeneration.load(std::memory_order_acquire);
		if (replay.resourceGeneration != resourceGeneration) {
			// Never call GetDevice on a retained device child. Frame-generation
			// proxies have returned an invalid COM device interface from that
			// method and crashed during Release. The renderer-owned generation
			// is the safe device-rebuild authority.
			for (auto& buffer : replay.vertexConstantBuffers) {
				buffer.Reset();
			}
			replay.resourceGeneration = resourceGeneration;
		}
		constexpr std::array<UINT, 3> constantBufferSlots{ 1U, 2U, 12U };
		for (std::size_t index = 0;
			index < constantBufferSlots.size();
			++index) {
			ComPtr<ID3D11Buffer> sourceBuffer;
			context->VSGetConstantBuffers(
				constantBufferSlots[index],
				1,
				sourceBuffer.GetAddressOf());
			if (!sourceBuffer.Get()) {
				return false;
			}

			D3D11_BUFFER_DESC sourceDescription{};
			sourceBuffer->GetDesc(&sourceDescription);
			bool recreate =
				!replay.vertexConstantBuffers[index].Get();
			if (!recreate) {
				D3D11_BUFFER_DESC destinationDescription{};
				replay.vertexConstantBuffers[index]->GetDesc(
					&destinationDescription);
				recreate =
					destinationDescription.ByteWidth !=
						sourceDescription.ByteWidth;
			}
			if (recreate) {
				replay.vertexConstantBuffers[index].Reset();
				D3D11_BUFFER_DESC copyDescription = sourceDescription;
				copyDescription.Usage = D3D11_USAGE_DEFAULT;
				copyDescription.CPUAccessFlags = 0;
				const HRESULT createResult = device->CreateBuffer(
					&copyDescription,
					nullptr,
					replay.vertexConstantBuffers[index]
						.ReleaseAndGetAddressOf());
				if (FAILED(createResult) ||
					!replay.vertexConstantBuffers[index].Get()) {
					return false;
				}
			}
			context->CopyResource(
				replay.vertexConstantBuffers[index].Get(),
				sourceBuffer.Get());
		}

		replay.vertexShader = vertexShader;
		replay.inputLayout = inputLayout;
		replay.vertexBuffer = vertexBuffer;
		replay.indexBuffer = indexBuffer;
		replay.vertexStride = vertexStride;
		replay.vertexOffset = vertexOffset;
		replay.indexFormat = indexFormat;
		replay.indexOffset = indexOffset;
		replay.indexCount = indexCount;
		replay.startIndexLocation = startIndexLocation;
		replay.baseVertexLocation = baseVertexLocation;
		replay.topology = topology;
		replay.generation =
			automaticSTSReplayFrameGeneration.load(
				std::memory_order_acquire);
		replay.ready = true;
		return true;
	}

	bool D3D::ReplayAutomaticSTSScopeFade(
		ID3D11ShaderResourceView* sceneSource,
		ID3D11RenderTargetView* compositeTarget)
	{
		if (!g_Context.Get()) {
			return false;
		}

		std::scoped_lock lock(mScopeFadeGeometryMutex);
		auto& replay = mAutomaticSTSScopeFadeReplay;
		if (!replay.ready || !replay.vertexShader.Get() ||
			!replay.inputLayout.Get() || !replay.vertexBuffer.Get() ||
			!replay.indexBuffer.Get() ||
			replay.generation == 0U ||
			replay.generation !=
				automaticSTSReplayFrameGeneration.load(
					std::memory_order_acquire) ||
			!m_pGeometryShader_STSGeometryFill.Get() ||
			!m_pPixelShader_STSGeometryMagnify.Get() ||
			!BSScopeFadeReplaceRGB.Get()) {
			return false;
		}

		ComPtr<ID3D11RenderTargetView> boundTarget;
		if (!compositeTarget) {
			g_Context->OMGetRenderTargets(
				1,
				boundTarget.GetAddressOf(),
				nullptr);
			compositeTarget = boundTarget.Get();
		}
		if (!compositeTarget ||
			!PrepareScopeFadeSceneSource(
				g_Context.Get(),
				compositeTarget,
				sceneSource)) {
			return false;
		}
		ID3D11ShaderResourceView* const effectiveSceneSource =
			sceneSource ? sceneSource : mScopeFadeSceneSRV.Get();
		if (!effectiveSceneSource) {
			return false;
		}

		std::vector<VSConstantBufferSlot> vertexConstantBufferSlots{
			{ 1U, 1U, replay.vertexConstantBuffers[0].GetAddressOf() },
			{ 2U, 1U, replay.vertexConstantBuffers[1].GetAddressOf() },
			{ 12U, 1U, replay.vertexConstantBuffers[2].GetAddressOf() }
		};
		ID3D11Buffer* vertexBuffer = replay.vertexBuffer.Get();
		const UINT vertexStride = replay.vertexStride;
		const UINT vertexOffset = replay.vertexOffset;
		SetupCommonRenderState(
			replay.vertexShader.Get(),
			nullptr,
			0,
			m_pPixelShader_STSGeometryMagnify.Get(),
			replay.inputLayout.Get(),
			BSScopeFadeReplaceRGB.Get(),
			vertexConstantBufferSlots,
			replay.indexBuffer.Get(),
			replay.indexFormat,
			replay.indexOffset,
			&vertexBuffer,
			&vertexStride,
			&vertexOffset,
			1,
			renderedAtTAAThisFrame ? nullptr : compositeTarget);

		g_Context->IASetPrimitiveTopology(replay.topology);
		g_Context->GSSetShader(
			m_pGeometryShader_STSGeometryFill.Get(),
			nullptr,
			0);
		ID3D11ShaderResourceView* source = effectiveSceneSource;
		ID3D11SamplerState* sampler = mScopeFadeSampler.Get();
		ID3D11Buffer* resolutionBuffer =
			mScopeFadeResolutionBuffer.Get();
		// b5 carries every depth and separation control the magnify shader
		// reads: ScopeSceneDepth, ScopeShadowDepth, ScopeImageStillness,
		// ScopeAxialBreathing, and ScopeApertureScaleRatio. Only b4 was bound
		// here, so the shader sampled whatever the game happened to leave in
		// slot 5. Shadow depth reading as zero collapsed the exit-pupil
		// displacement to nothing, which is why no eye-box setting could ever
		// produce a crescent, and the same omission silently disabled scene
		// parallax and image stillness. The WARP harnesses bind b5 explicitly,
		// so they could not catch it.
		ID3D11Buffer* scopeEffectBuffer = m_pScopeEffectBuffer.Get();
		g_Context->PSSetShaderResources(4, 1, &source);
		g_Context->PSSetSamplers(0, 1, &sampler);
		g_Context->PSSetConstantBuffers(4, 1, &resolutionBuffer);
		if (scopeEffectBuffer) {
			g_Context->PSSetConstantBuffers(5, 1, &scopeEffectBuffer);
		}

		bSelfDraw = true;
		g_Context->DrawIndexed(
			replay.indexCount,
			replay.startIndexLocation,
			replay.baseVertexLocation);
		bSelfDraw = false;
		replay.ready = false;
		return true;
	}

	bool D3D::CaptureAutomaticSTSReticleReplay(
		ID3D11DeviceContext* context,
		UINT indexCount,
		UINT startIndexLocation,
		INT baseVertexLocation)
	{
		if (!context || indexCount == 0U ||
			context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
			!g_Context.Get() ||
			!HaveSameCOMIdentity(context, g_Context.Get())) {
			return false;
		}

		auto& replay = mAutomaticSTSReticleReplay;
		replay.ready = false;
		ComPtr<ID3D11Device> device;
		context->GetDevice(device.GetAddressOf());
		if (!device.Get() || !g_Device.Get() ||
			!HaveSameCOMIdentity(device.Get(), g_Device.Get())) {
			return false;
		}

		context->VSGetShader(
			replay.vertexShader.ReleaseAndGetAddressOf(), nullptr, nullptr);
		context->GSGetShader(
			replay.geometryShader.ReleaseAndGetAddressOf(), nullptr, nullptr);
		context->PSGetShader(
			replay.pixelShader.ReleaseAndGetAddressOf(), nullptr, nullptr);
		context->IAGetInputLayout(replay.inputLayout.ReleaseAndGetAddressOf());
		context->IAGetPrimitiveTopology(&replay.topology);
		context->IAGetVertexBuffers(
			0U,
			1U,
			replay.vertexBuffer.ReleaseAndGetAddressOf(),
			&replay.vertexStride,
			&replay.vertexOffset);
		context->IAGetIndexBuffer(
			replay.indexBuffer.ReleaseAndGetAddressOf(),
			&replay.indexFormat,
			&replay.indexOffset);
		if (!replay.vertexShader.Get() || !replay.pixelShader.Get() ||
			!replay.inputLayout.Get() || !replay.vertexBuffer.Get() ||
			!replay.indexBuffer.Get() || replay.vertexStride == 0U ||
			replay.topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST ||
			(replay.indexFormat != DXGI_FORMAT_R16_UINT &&
				replay.indexFormat != DXGI_FORMAT_R32_UINT)) {
			return false;
		}

		const auto resourceGeneration =
			mScopeFadeResourceGeneration.load(std::memory_order_acquire);
		if (replay.resourceGeneration != resourceGeneration) {
			for (auto& buffer : replay.vertexConstantBuffers) {
				buffer.Reset();
			}
			for (auto& buffer : replay.pixelConstantBuffers) {
				buffer.Reset();
			}
			replay.depthDisabledState.Reset();
			replay.resourceGeneration = resourceGeneration;
		}

		// Fallout reuses dynamic material and transform constant buffers later
		// in the frame. Copy every bound slot now so the late replay observes
		// exactly the values used by the authored reticle draw.
		auto copyConstantBuffer = [&](bool vertexStage,
			UINT slot,
			ComPtr<ID3D11Buffer>& destination) {
			ComPtr<ID3D11Buffer> source;
			if (vertexStage) {
				context->VSGetConstantBuffers(
					slot, 1U, source.GetAddressOf());
			} else {
				context->PSGetConstantBuffers(
					slot, 1U, source.GetAddressOf());
			}
			if (!source.Get()) {
				destination.Reset();
				return true;
			}

			D3D11_BUFFER_DESC sourceDescription{};
			source->GetDesc(&sourceDescription);
			bool recreate = !destination.Get();
			if (!recreate) {
				D3D11_BUFFER_DESC destinationDescription{};
				destination->GetDesc(&destinationDescription);
				recreate =
					destinationDescription.ByteWidth !=
						sourceDescription.ByteWidth;
			}
			if (recreate) {
				destination.Reset();
				D3D11_BUFFER_DESC copyDescription = sourceDescription;
				copyDescription.Usage = D3D11_USAGE_DEFAULT;
				copyDescription.CPUAccessFlags = 0U;
				if (FAILED(device->CreateBuffer(
						&copyDescription,
						nullptr,
						destination.ReleaseAndGetAddressOf())) ||
					!destination.Get()) {
					return false;
				}
			}
			context->CopyResource(destination.Get(), source.Get());
			return true;
		};
		for (UINT slot = 0U; slot < 14U; ++slot) {
			if (!copyConstantBuffer(
					true, slot, replay.vertexConstantBuffers[slot]) ||
				!copyConstantBuffer(
					false, slot, replay.pixelConstantBuffers[slot])) {
				return false;
			}
		}

		for (UINT slot = 0U; slot < 16U; ++slot) {
			context->VSGetShaderResources(
				slot,
				1U,
				replay.vertexShaderResources[slot]
					.ReleaseAndGetAddressOf());
			context->PSGetShaderResources(
				slot,
				1U,
				replay.pixelShaderResources[slot]
					.ReleaseAndGetAddressOf());
			context->VSGetSamplers(
				slot,
				1U,
				replay.vertexSamplers[slot].ReleaseAndGetAddressOf());
			context->PSGetSamplers(
				slot,
				1U,
				replay.pixelSamplers[slot].ReleaseAndGetAddressOf());
		}
		context->RSGetState(replay.rasterizerState.ReleaseAndGetAddressOf());
		replay.viewportCount =
			D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		context->RSGetViewports(
			&replay.viewportCount, replay.viewports.data());
		replay.scissorCount =
			D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		context->RSGetScissorRects(
			&replay.scissorCount, replay.scissorRects.data());
		context->OMGetBlendState(
			replay.blendState.ReleaseAndGetAddressOf(),
			replay.blendFactor.data(),
			&replay.sampleMask);

		if (!replay.depthDisabledState.Get()) {
			D3D11_DEPTH_STENCIL_DESC depthDescription{};
			depthDescription.DepthEnable = false;
			depthDescription.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			depthDescription.DepthFunc = D3D11_COMPARISON_ALWAYS;
			depthDescription.StencilEnable = false;
			if (FAILED(device->CreateDepthStencilState(
					&depthDescription,
					replay.depthDisabledState.ReleaseAndGetAddressOf())) ||
				!replay.depthDisabledState.Get()) {
				return false;
			}
		}

		replay.indexCount = indexCount;
		replay.startIndexLocation = startIndexLocation;
		replay.baseVertexLocation = baseVertexLocation;
		replay.generation =
			automaticSTSReplayFrameGeneration.load(
				std::memory_order_acquire);
		replay.ready = true;
		return true;
	}

	bool D3D::ReplayAutomaticSTSReticle(
		ID3D11RenderTargetView* compositeTarget)
	{
		std::scoped_lock lock(mAutomaticSTSReticleReplayMutex);
		auto& replay = mAutomaticSTSReticleReplay;
		if (!g_Context.Get() || !replay.ready ||
			replay.generation == 0U ||
			replay.generation !=
				automaticSTSReplayFrameGeneration.load(
					std::memory_order_acquire) ||
			replay.resourceGeneration !=
				mScopeFadeResourceGeneration.load(
					std::memory_order_acquire) ||
			!replay.vertexShader.Get() || !replay.pixelShader.Get() ||
			!replay.inputLayout.Get() || !replay.vertexBuffer.Get() ||
			!replay.indexBuffer.Get() || !replay.depthDisabledState.Get()) {
			return false;
		}

		ComPtr<ID3D11RenderTargetView> boundTarget;
		if (!compositeTarget) {
			g_Context->OMGetRenderTargets(
				1U, boundTarget.GetAddressOf(), nullptr);
			compositeTarget = boundTarget.Get();
		}
		if (!compositeTarget) {
			return false;
		}

		ID3D11RenderTargetView* target = compositeTarget;
		g_Context->OMSetRenderTargets(1U, &target, nullptr);
		g_Context->OMSetBlendState(
			replay.blendState.Get(),
			replay.blendFactor.data(),
			replay.sampleMask);
		g_Context->OMSetDepthStencilState(
			replay.depthDisabledState.Get(), 0U);
		g_Context->RSSetState(replay.rasterizerState.Get());
		if (replay.viewportCount > 0U) {
			g_Context->RSSetViewports(
				replay.viewportCount, replay.viewports.data());
		}
		if (replay.scissorCount > 0U) {
			g_Context->RSSetScissorRects(
				replay.scissorCount, replay.scissorRects.data());
		}

		ID3D11Buffer* vertexBuffer = replay.vertexBuffer.Get();
		g_Context->IASetInputLayout(replay.inputLayout.Get());
		g_Context->IASetVertexBuffers(
			0U,
			1U,
			&vertexBuffer,
			&replay.vertexStride,
			&replay.vertexOffset);
		g_Context->IASetIndexBuffer(
			replay.indexBuffer.Get(),
			replay.indexFormat,
			replay.indexOffset);
		g_Context->IASetPrimitiveTopology(replay.topology);
		g_Context->VSSetShader(replay.vertexShader.Get(), nullptr, 0U);
		g_Context->GSSetShader(replay.geometryShader.Get(), nullptr, 0U);
		g_Context->PSSetShader(replay.pixelShader.Get(), nullptr, 0U);

		std::array<ID3D11Buffer*, 14> vertexConstants{};
		std::array<ID3D11Buffer*, 14> pixelConstants{};
		for (std::size_t slot = 0; slot < vertexConstants.size(); ++slot) {
			vertexConstants[slot] = replay.vertexConstantBuffers[slot].Get();
			pixelConstants[slot] = replay.pixelConstantBuffers[slot].Get();
		}
		g_Context->VSSetConstantBuffers(
			0U, static_cast<UINT>(vertexConstants.size()),
			vertexConstants.data());
		g_Context->PSSetConstantBuffers(
			0U, static_cast<UINT>(pixelConstants.size()),
			pixelConstants.data());

		std::array<ID3D11ShaderResourceView*, 16> vertexResources{};
		std::array<ID3D11ShaderResourceView*, 16> pixelResources{};
		std::array<ID3D11SamplerState*, 16> vertexSamplers{};
		std::array<ID3D11SamplerState*, 16> pixelSamplers{};
		for (std::size_t slot = 0; slot < vertexResources.size(); ++slot) {
			vertexResources[slot] = replay.vertexShaderResources[slot].Get();
			pixelResources[slot] = replay.pixelShaderResources[slot].Get();
			vertexSamplers[slot] = replay.vertexSamplers[slot].Get();
			pixelSamplers[slot] = replay.pixelSamplers[slot].Get();
		}
		g_Context->VSSetShaderResources(
			0U, static_cast<UINT>(vertexResources.size()),
			vertexResources.data());
		g_Context->PSSetShaderResources(
			0U, static_cast<UINT>(pixelResources.size()),
			pixelResources.data());
		g_Context->VSSetSamplers(
			0U, static_cast<UINT>(vertexSamplers.size()),
			vertexSamplers.data());
		g_Context->PSSetSamplers(
			0U, static_cast<UINT>(pixelSamplers.size()),
			pixelSamplers.data());

		bSelfDraw = true;
		g_Context->DrawIndexed(
			replay.indexCount,
			replay.startIndexLocation,
			replay.baseVertexLocation);
		bSelfDraw = false;
		replay.ready = false;
		return true;
	}

	void D3D::ClearAutomaticSTSReticleReplay() noexcept
	{
		try {
			std::scoped_lock lock(mAutomaticSTSReticleReplayMutex);
			mAutomaticSTSReticleReplay.ready = false;
			mAutomaticSTSReticleReplay.generation = 0U;
		} catch (...) {
			mAutomaticSTSReticleReplay.ready = false;
		}
	}

	bool D3D::BeginAutomaticSTSReticleLayerCapture(
		ID3D11DeviceContext* context,
		ID3D11RenderTargetView* sourceTarget,
		ID3D11DepthStencilView* sourceDepth,
		bool whiteBackground)
	{
		if (!context || !sourceTarget) {
			return false;
		}
		if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
			!g_Context.Get() ||
			!HaveSameCOMIdentity(context, g_Context.Get())) {
			return false;
		}

		ComPtr<ID3D11Device> device;
		context->GetDevice(device.GetAddressOf());
		if (!device.Get() || !g_Device.Get() ||
			!HaveSameCOMIdentity(device.Get(), g_Device.Get()) ||
			(mScopeFadeResourceDevice.Get() &&
				!HaveSameCOMIdentity(
					device.Get(),
					mScopeFadeResourceDevice.Get()))) {
			return false;
		}

		ComPtr<ID3D11Resource> sourceResource;
		sourceTarget->GetResource(sourceResource.GetAddressOf());
		ComPtr<ID3D11Texture2D> sourceTexture;
		if (!sourceResource.Get() ||
			FAILED(sourceResource.As(&sourceTexture)) ||
			!sourceTexture.Get()) {
			return false;
		}

		D3D11_TEXTURE2D_DESC sourceDescription{};
		sourceTexture->GetDesc(&sourceDescription);
		D3D11_RENDER_TARGET_VIEW_DESC sourceViewDescription{};
		sourceTarget->GetDesc(&sourceViewDescription);
		if (sourceDescription.Width == 0U ||
			sourceDescription.Height == 0U ||
			sourceDescription.SampleDesc.Count != 1U ||
			sourceViewDescription.ViewDimension !=
				D3D11_RTV_DIMENSION_TEXTURE2D) {
			// The simple transparent layer uses a regular Texture2D SRV.
			// Fail closed on an unexpected MSAA target rather than binding an
			// incompatible view during ADS.
			return false;
		}

		// The private layer reproduces displayed color target zero. A separate
		// color-suppressed authored replay below preserves depth/stencil and any
		// auxiliary MRT outputs, so multiple bound targets are supported rather
		// than forcing the reticle back into the magnified scene.
		ID3D11RenderTargetView* boundTargets[
			D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		context->OMGetRenderTargets(
			D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
			boundTargets,
			nullptr);
		const bool targetZeroMatches = boundTargets[0] &&
			HaveSameCOMIdentity(boundTargets[0], sourceTarget);
		for (auto*& target : boundTargets) {
			if (target) {
				target->Release();
				target = nullptr;
			}
		}
		if (!targetZeroMatches) {
			return false;
		}

		// Paired black/white reconstruction is exact for the standard affine
		// ADD blend families used by STS reticles. Destination-dependent,
		// dual-source, MIN/MAX, and subtractive authored blends are deliberately
		// left to the ordinary STS path rather than approximated.
		ComPtr<ID3D11BlendState> authoredBlendState;
		float authoredBlendFactor[4]{};
		UINT authoredSampleMask = D3D11_DEFAULT_SAMPLE_MASK;
		context->OMGetBlendState(
			authoredBlendState.GetAddressOf(),
			authoredBlendFactor,
			&authoredSampleMask);
		if (authoredBlendState.Get()) {
			D3D11_BLEND_DESC authoredBlendDescription{};
			authoredBlendState->GetDesc(&authoredBlendDescription);
			const auto& authoredTarget =
				authoredBlendDescription.RenderTarget[0];
			const auto isSourceOnlyFactor = [](D3D11_BLEND factor) {
				switch (factor) {
				case D3D11_BLEND_ZERO:
				case D3D11_BLEND_ONE:
				case D3D11_BLEND_SRC_COLOR:
				case D3D11_BLEND_INV_SRC_COLOR:
				case D3D11_BLEND_SRC_ALPHA:
				case D3D11_BLEND_INV_SRC_ALPHA:
				case D3D11_BLEND_BLEND_FACTOR:
				case D3D11_BLEND_INV_BLEND_FACTOR:
					return true;
				default:
					return false;
				}
			};
			if (authoredTarget.BlendEnable &&
				(authoredTarget.BlendOp != D3D11_BLEND_OP_ADD ||
					!isSourceOnlyFactor(authoredTarget.SrcBlend) ||
					!isSourceOnlyFactor(authoredTarget.DestBlend))) {
				return false;
			}
		}

		// Private captures reconstruct the complete reticle surface. They must
		// not inherit the live first-person depth buffer: at extreme pitch the
		// scope housing can occlude part of the reticle mesh before the late
		// ScopeFade aperture clip gets a chance to constrain it. The authored
		// draw is replayed once afterward with displayed color suppressed, so
		// Fallout still receives its original depth/stencil mutations exactly
		// once.

		const auto resourceGeneration =
			mScopeFadeResourceGeneration.load(std::memory_order_acquire);
		if (mAutomaticSTSReticleLayerResourceGeneration !=
			resourceGeneration) {
			mAutomaticSTSReticleLayerWhiteSRV.Reset();
			mAutomaticSTSReticleLayerWhiteRTV.Reset();
			mAutomaticSTSReticleLayerWhiteTexture.Reset();
			mAutomaticSTSReticleLayerSRV.Reset();
			mAutomaticSTSReticleLayerRTV.Reset();
			mAutomaticSTSReticleLayerTexture.Reset();
			mAutomaticSTSReticleLayerCompositeBlend.Reset();
			mAutomaticSTSReticleSuppressionSourceBlend.Reset();
			mAutomaticSTSReticleColorSuppressionBlend.Reset();
			mAutomaticSTSReticleSuppressionSourceWasNull = false;
			mAutomaticSTSReticleLayerReadOnlyDepthState.Reset();
			mAutomaticSTSReticleLayerCaptureGeneration = 0U;
			mAutomaticSTSReticleLayerResourceGeneration =
				resourceGeneration;
		}

		bool recreate =
			!mAutomaticSTSReticleLayerTexture.Get() ||
			!mAutomaticSTSReticleLayerRTV.Get() ||
			!mAutomaticSTSReticleLayerSRV.Get() ||
			!mAutomaticSTSReticleLayerWhiteTexture.Get() ||
			!mAutomaticSTSReticleLayerWhiteRTV.Get() ||
			!mAutomaticSTSReticleLayerWhiteSRV.Get();
		if (!recreate) {
			D3D11_TEXTURE2D_DESC layerDescription{};
			mAutomaticSTSReticleLayerTexture->GetDesc(&layerDescription);
			D3D11_RENDER_TARGET_VIEW_DESC layerViewDescription{};
			mAutomaticSTSReticleLayerRTV->GetDesc(&layerViewDescription);
			recreate =
				layerDescription.Width != sourceDescription.Width ||
				layerDescription.Height != sourceDescription.Height ||
				layerDescription.Format != sourceDescription.Format ||
				layerViewDescription.Format !=
					sourceViewDescription.Format;
		}

		if (recreate) {
			mAutomaticSTSReticleLayerWhiteSRV.Reset();
			mAutomaticSTSReticleLayerWhiteRTV.Reset();
			mAutomaticSTSReticleLayerWhiteTexture.Reset();
			mAutomaticSTSReticleLayerSRV.Reset();
			mAutomaticSTSReticleLayerRTV.Reset();
			mAutomaticSTSReticleLayerTexture.Reset();

			D3D11_TEXTURE2D_DESC layerDescription{};
			layerDescription.Width = sourceDescription.Width;
			layerDescription.Height = sourceDescription.Height;
			layerDescription.MipLevels = 1U;
			layerDescription.ArraySize = 1U;
			// Preserve the live target's resource and view formats. STS reticle
			// shaders were authored against Fallout's current color target; a
			// hard-coded RGBA8 layer can reinterpret HDR/sRGB output and is not a
			// valid substitute through ENB or upscaler proxy devices.
			layerDescription.Format = sourceDescription.Format;
			layerDescription.SampleDesc.Count = 1U;
			layerDescription.SampleDesc.Quality = 0U;
			layerDescription.Usage = D3D11_USAGE_DEFAULT;
			layerDescription.BindFlags =
				D3D11_BIND_RENDER_TARGET |
				D3D11_BIND_SHADER_RESOURCE;
			layerDescription.CPUAccessFlags = 0U;
			layerDescription.MiscFlags = 0U;
			D3D11_RENDER_TARGET_VIEW_DESC layerRTVDescription =
				sourceViewDescription;
			layerRTVDescription.Texture2D.MipSlice = 0U;
			D3D11_SHADER_RESOURCE_VIEW_DESC layerSRVDescription{};
			layerSRVDescription.Format = sourceViewDescription.Format;
			layerSRVDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			layerSRVDescription.Texture2D.MostDetailedMip = 0U;
			layerSRVDescription.Texture2D.MipLevels = 1U;
			if (FAILED(device->CreateTexture2D(
					&layerDescription,
					nullptr,
					mAutomaticSTSReticleLayerTexture
						.ReleaseAndGetAddressOf())) ||
				FAILED(device->CreateRenderTargetView(
					mAutomaticSTSReticleLayerTexture.Get(),
					&layerRTVDescription,
					mAutomaticSTSReticleLayerRTV
						.ReleaseAndGetAddressOf())) ||
				FAILED(device->CreateShaderResourceView(
					mAutomaticSTSReticleLayerTexture.Get(),
					&layerSRVDescription,
					mAutomaticSTSReticleLayerSRV
						.ReleaseAndGetAddressOf())) ||
				FAILED(device->CreateTexture2D(
					&layerDescription,
					nullptr,
					mAutomaticSTSReticleLayerWhiteTexture
						.ReleaseAndGetAddressOf())) ||
				FAILED(device->CreateRenderTargetView(
					mAutomaticSTSReticleLayerWhiteTexture.Get(),
					&layerRTVDescription,
					mAutomaticSTSReticleLayerWhiteRTV
						.ReleaseAndGetAddressOf())) ||
				FAILED(device->CreateShaderResourceView(
					mAutomaticSTSReticleLayerWhiteTexture.Get(),
					&layerSRVDescription,
					mAutomaticSTSReticleLayerWhiteSRV
						.ReleaseAndGetAddressOf()))) {
				mAutomaticSTSReticleLayerWhiteSRV.Reset();
				mAutomaticSTSReticleLayerWhiteRTV.Reset();
				mAutomaticSTSReticleLayerWhiteTexture.Reset();
				mAutomaticSTSReticleLayerSRV.Reset();
				mAutomaticSTSReticleLayerRTV.Reset();
				mAutomaticSTSReticleLayerTexture.Reset();
				return false;
			}
		}
		if (!mAutomaticSTSReticleLayerCompositeBlend.Get()) {
			// The reticle composite shader supplies the black-background result
			// through SV_Target0 and the white-minus-black transmittance through
			// SV_Target1. Dual-source blending evaluates B + D*T in one draw,
			// reproducing the authored affine blend without an alpha guess.
			D3D11_BLEND_DESC blendDescription{};
			auto& targetBlend = blendDescription.RenderTarget[0];
			targetBlend.BlendEnable = TRUE;
			targetBlend.SrcBlend = D3D11_BLEND_ONE;
			targetBlend.DestBlend = D3D11_BLEND_SRC1_COLOR;
			targetBlend.BlendOp = D3D11_BLEND_OP_ADD;
			// The displayed color buffer's alpha is not part of the reticle
			// appearance. Preserve it exactly so the composite cannot disturb a
			// later ENB, upscaler, or frame-generation consumer.
			targetBlend.SrcBlendAlpha = D3D11_BLEND_ZERO;
			targetBlend.DestBlendAlpha = D3D11_BLEND_ONE;
			targetBlend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			targetBlend.RenderTargetWriteMask =
				D3D11_COLOR_WRITE_ENABLE_ALL;
			if (FAILED(device->CreateBlendState(
					&blendDescription,
					mAutomaticSTSReticleLayerCompositeBlend
						.ReleaseAndGetAddressOf()))) {
				return false;
			}
		}

		const auto frameGeneration =
			automaticSTSReplayFrameGeneration.load(
				std::memory_order_acquire);
		const bool firstCaptureThisFrame =
			mAutomaticSTSReticleLayerCaptureGeneration != frameGeneration;
		if (firstCaptureThisFrame) {
			// Captured alpha is intentionally untrusted. Both backgrounds use
			// alpha one so the reconstruction contract is derived exclusively
			// from the paired RGB response of the authored blend state.
			constexpr float opaqueBlack[4]{
				0.0F,
				0.0F,
				0.0F,
				1.0F
			};
			constexpr float opaqueWhite[4]{
				1.0F,
				1.0F,
				1.0F,
				1.0F
			};
			context->ClearRenderTargetView(
				mAutomaticSTSReticleLayerRTV.Get(),
				opaqueBlack);
			context->ClearRenderTargetView(
				mAutomaticSTSReticleLayerWhiteRTV.Get(),
				opaqueWhite);
			mAutomaticSTSReticleLayerReady = false;
			mAutomaticSTSReticleLayerCaptureGeneration = frameGeneration;
		}
		ID3D11RenderTargetView* const layerTarget =
			whiteBackground ?
				mAutomaticSTSReticleLayerWhiteRTV.Get() :
				mAutomaticSTSReticleLayerRTV.Get();
		context->OMSetRenderTargets(1U, &layerTarget, nullptr);

		// The private target has no depth attachment, and an explicit disabled
		// state avoids inheriting a depth/stencil contract from the live draw.
		if (!mAutomaticSTSReticleLayerReadOnlyDepthState.Get()) {
			D3D11_DEPTH_STENCIL_DESC readOnlyDescription{};
			readOnlyDescription.DepthEnable = FALSE;
			readOnlyDescription.DepthWriteMask =
				D3D11_DEPTH_WRITE_MASK_ZERO;
			readOnlyDescription.DepthFunc = D3D11_COMPARISON_ALWAYS;
			readOnlyDescription.StencilEnable = FALSE;
			readOnlyDescription.StencilReadMask =
				D3D11_DEFAULT_STENCIL_READ_MASK;
			readOnlyDescription.StencilWriteMask = 0U;
			mAutomaticSTSReticleLayerReadOnlyDepthState.Reset();
			if (FAILED(device->CreateDepthStencilState(
					&readOnlyDescription,
					mAutomaticSTSReticleLayerReadOnlyDepthState
						.ReleaseAndGetAddressOf()))) {
				return false;
			}
		}
		context->OMSetDepthStencilState(
			mAutomaticSTSReticleLayerReadOnlyDepthState.Get(),
			0U);
		// Preserve the complete authored draw contract, including its original
		// blend state. The paired backgrounds let the late pass recover both the
		// source contribution and destination transmittance, so no capture-time
		// state replacement is necessary.
		return true;
	}

	bool D3D::ApplyAutomaticSTSReticleColorSuppression(
		ID3D11DeviceContext* context)
	{
		if (!context || context->GetType() !=
			D3D11_DEVICE_CONTEXT_IMMEDIATE) {
			return false;
		}
		ComPtr<ID3D11Device> device;
		context->GetDevice(device.GetAddressOf());
		if (!device.Get() || !g_Device.Get() ||
			!HaveSameCOMIdentity(device.Get(), g_Device.Get())) {
			return false;
		}

		ComPtr<ID3D11BlendState> authoredBlend;
		float blendFactor[4]{};
		UINT sampleMask = D3D11_DEFAULT_SAMPLE_MASK;
		context->OMGetBlendState(
			authoredBlend.GetAddressOf(), blendFactor, &sampleMask);
		const bool authoredWasNull = !authoredBlend.Get();
		const bool cachedSourceMatches = authoredWasNull ?
			(mAutomaticSTSReticleSuppressionSourceWasNull &&
				!mAutomaticSTSReticleSuppressionSourceBlend.Get()) :
			(mAutomaticSTSReticleSuppressionSourceBlend.Get() &&
				HaveSameCOMIdentity(
					authoredBlend.Get(),
					mAutomaticSTSReticleSuppressionSourceBlend.Get()));
		if (!cachedSourceMatches ||
			!mAutomaticSTSReticleColorSuppressionBlend.Get()) {
			D3D11_BLEND_DESC suppressedDescription{};
			if (authoredBlend.Get()) {
				authoredBlend->GetDesc(&suppressedDescription);
			} else {
				// D3D11's null blend state uses the ordinary opaque defaults.
				auto& target = suppressedDescription.RenderTarget[0];
				target.BlendEnable = FALSE;
				target.SrcBlend = D3D11_BLEND_ONE;
				target.DestBlend = D3D11_BLEND_ZERO;
				target.BlendOp = D3D11_BLEND_OP_ADD;
				target.SrcBlendAlpha = D3D11_BLEND_ONE;
				target.DestBlendAlpha = D3D11_BLEND_ZERO;
				target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
				target.RenderTargetWriteMask =
					D3D11_COLOR_WRITE_ENABLE_ALL;
			}
			if (!suppressedDescription.IndependentBlendEnable) {
				// Enabling independent state must preserve the authored RT0
				// contract for every auxiliary target before RT0 is muted.
				for (UINT slot = 1U;
					slot < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
					++slot) {
					suppressedDescription.RenderTarget[slot] =
						suppressedDescription.RenderTarget[0];
				}
				suppressedDescription.IndependentBlendEnable = TRUE;
			}
			suppressedDescription.RenderTarget[0].RenderTargetWriteMask = 0U;
			mAutomaticSTSReticleColorSuppressionBlend.Reset();
			if (FAILED(device->CreateBlendState(
					&suppressedDescription,
					mAutomaticSTSReticleColorSuppressionBlend
						.ReleaseAndGetAddressOf()))) {
				mAutomaticSTSReticleSuppressionSourceBlend.Reset();
				mAutomaticSTSReticleSuppressionSourceWasNull = false;
				return false;
			}
			mAutomaticSTSReticleSuppressionSourceBlend = authoredBlend;
			mAutomaticSTSReticleSuppressionSourceWasNull = authoredWasNull;
		}
		context->OMSetBlendState(
			mAutomaticSTSReticleColorSuppressionBlend.Get(),
			blendFactor,
			sampleMask);
		return true;
	}

	std::unique_lock<std::mutex>
	D3D::LockAutomaticSTSReticleLayerCapture()
	{
		return std::unique_lock<std::mutex>(
			mAutomaticSTSReticleLayerMutex);
	}

	void D3D::CompleteAutomaticSTSReticleLayerCapture(
		bool captured,
		std::uint64_t frameGeneration) noexcept
	{
		if (!captured) {
			// A compound ReticleNode can submit several geometry draws. A
			// later unsupported leaf must fail open without discarding earlier
			// leaves that were already captured and color-suppressed this frame.
			if (mAutomaticSTSReticleLayerReady &&
				mAutomaticSTSReticleLayerGeneration == frameGeneration) {
				return;
			}
			mAutomaticSTSReticleLayerReady = false;
			mAutomaticSTSReticleLayerCaptureGeneration = 0U;
			return;
		}
		mAutomaticSTSReticleLayerGeneration = frameGeneration;
		mAutomaticSTSReticleLayerResourceGeneration =
			mScopeFadeResourceGeneration.load(
				std::memory_order_acquire);
		mAutomaticSTSReticleLayerReady =
			mAutomaticSTSReticleLayerReady ||
			(mAutomaticSTSReticleLayerSRV.Get() &&
				mAutomaticSTSReticleLayerWhiteSRV.Get());
	}

	bool D3D::CompositeAutomaticSTSReticleLayer(
		ID3D11RenderTargetView* compositeTarget)
	{
		std::scoped_lock lock(mAutomaticSTSReticleLayerMutex);
		ComPtr<ID3D11RenderTargetView> boundTarget;
		if (!compositeTarget && g_Context.Get()) {
			g_Context->OMGetRenderTargets(
				1U,
				boundTarget.GetAddressOf(),
				nullptr);
			compositeTarget = boundTarget.Get();
		}
		if (!mAutomaticSTSReticleLayerReady ||
			mAutomaticSTSReticleLayerGeneration == 0U ||
			mAutomaticSTSReticleLayerGeneration !=
				automaticSTSReplayFrameGeneration.load(
					std::memory_order_acquire) ||
			mAutomaticSTSReticleLayerResourceGeneration !=
				mScopeFadeResourceGeneration.load(
					std::memory_order_acquire) ||
			!mAutomaticSTSReticleLayerSRV.Get() ||
			!mAutomaticSTSReticleLayerWhiteSRV.Get() ||
			!mAutomaticSTSReticleLayerCompositeBlend.Get() ||
			!m_pPixelShader_STSReticleLayer.Get() ||
			!m_pVertexShader_Legacy.Get() || !compositeTarget) {
			return false;
		}

		const std::vector<VSConstantBufferSlot> noVertexConstants;
		UINT stride = sizeof(::Vertex);
		UINT offset = 0U;
		SetupCommonRenderState(
			m_pVertexShader_Legacy.Get(),
			nullptr,
			0U,
			m_pPixelShader_STSReticleLayer.Get(),
			gdc_pVertexLayout,
			mAutomaticSTSReticleLayerCompositeBlend.Get(),
			noVertexConstants,
			gdc_pIndexBuffer,
			DXGI_FORMAT_R32_UINT,
			0U,
			&gdc_pVertexBuffer,
			&stride,
			&offset,
			1U,
			compositeTarget);

		ID3D11ShaderResourceView* layerSources[2]{
			mAutomaticSTSReticleLayerSRV.Get(),
			mAutomaticSTSReticleLayerWhiteSRV.Get()
		};
		ID3D11SamplerState* sampler = mScopeFadeSampler.Get();
		ID3D11Buffer* resolutionBuffer =
			mScopeFadeResolutionBuffer.Get();
		// The reticle layer reads ScopeSceneDepth and ScopeShadowDepth from b5
		// to follow the optical image and match the scene's exit pupil. Without
		// this binding it tracked a zero depth and could never share the scene
		// shader's shadow.
		ID3D11Buffer* scopeEffectBuffer = m_pScopeEffectBuffer.Get();
		g_Context->PSSetShaderResources(4U, 2U, layerSources);
		g_Context->PSSetSamplers(0U, 1U, &sampler);
		g_Context->PSSetConstantBuffers(4U, 1U, &resolutionBuffer);
		if (scopeEffectBuffer) {
			g_Context->PSSetConstantBuffers(5U, 1U, &scopeEffectBuffer);
		}
		bSelfDraw = true;
		g_Context->DrawIndexed(3U, 0U, 0);
		bSelfDraw = false;
		ID3D11ShaderResourceView* nullSources[2]{ nullptr, nullptr };
		g_Context->PSSetShaderResources(4U, 2U, nullSources);
		mAutomaticSTSReticleLayerReady = false;
		return true;
	}

	void D3D::ClearAutomaticSTSReticleLayer() noexcept
	{
		std::scoped_lock lock(mAutomaticSTSReticleLayerMutex);
		mAutomaticSTSReticleLayerReady = false;
		mAutomaticSTSReticleLayerGeneration = 0U;
		mAutomaticSTSReticleLayerCaptureGeneration = 0U;
		// Per-frame invalidation must not discard size/device-dependent GPU
		// resources. ReleaseSizeDependentResources owns that lifecycle.
	}

	void D3D::ClearAutomaticSTSScopeFadeReplay() noexcept
	{
		try {
			std::scoped_lock lock(mScopeFadeGeometryMutex);
			mAutomaticSTSScopeFadeReplay = {};
		} catch (...) {
			// A cleanup failure must not escape an equip, load, or resize
			// boundary. Marking the replay unavailable is sufficient to fall
			// back to ordinary STS for the next frame.
			mAutomaticSTSScopeFadeReplay.ready = false;
		}
	}

	void D3D::CreateBlender()
	{
		// 1. Alpha-To-Coverage 混合状态
		D3D11_BLEND_DESC blendDesc = {};
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		HR(g_Device->CreateBlendState(&blendDesc, BSAlphaToCoverage.GetAddressOf()));

		// 2. 透明混合状态 (BSTransparent)
		blendDesc = {};
		blendDesc.AlphaToCoverageEnable = false;
		blendDesc.IndependentBlendEnable = false;
		auto& rtDesc = blendDesc.RenderTarget[0];
		rtDesc.BlendEnable = true;
		rtDesc.SrcBlend = D3D11_BLEND_SRC_ALPHA;
		rtDesc.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		rtDesc.BlendOp = D3D11_BLEND_OP_ADD;
		// The scope pass changes RGB only. Replacing destination alpha with
		// shader alpha wrote zero across the entire frame outside the lens,
		// which can invalidate ENB/upscaler composition metadata even though
		// the visible RGB looked unchanged.
		rtDesc.SrcBlendAlpha = D3D11_BLEND_ZERO;
		rtDesc.DestBlendAlpha = D3D11_BLEND_ONE;
		rtDesc.BlendOpAlpha = D3D11_BLEND_OP_ADD;
		rtDesc.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		HR(g_Device->CreateBlendState(&blendDesc, BSTransparent.GetAddressOf()));

	}

	void CreateConstantBuffer(ID3D11Device* device, ID3D11Buffer** buffer, UINT byteWidth)
	{
		D3D11_BUFFER_DESC desc = {};
		desc.Usage = D3D11_USAGE_DYNAMIC;
		// D3D11 requires constant-buffer allocations to be 16-byte aligned.
		// Keep the rounding even though the Stage 4e.2 data now occupies one
		// complete register; other call sites use this helper too.
		desc.ByteWidth = (byteWidth + 15U) & ~15U;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		HR(device->CreateBuffer(&desc, nullptr, buffer));
	}

	template <typename T>
	void CreateVertexBuffer(ID3D11Device* device, ID3D11Buffer** buffer, const T* data, UINT count)
	{
		D3D11_BUFFER_DESC desc = {};
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.ByteWidth = sizeof(T) * count;
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA initData = { data };
		HR(device->CreateBuffer(&desc, &initData, buffer));
	}

	template <typename T>
	void CreateIndexBuffer(ID3D11Device* device, ID3D11Buffer** buffer, const T* data, UINT count)
	{
		D3D11_BUFFER_DESC desc = {};
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.ByteWidth = sizeof(T) * count;
		desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
		D3D11_SUBRESOURCE_DATA initData = { data };
		HR(device->CreateBuffer(&desc, &initData, buffer));
	}

	bool D3D::InitResource()
	{
		// 常量缓冲区
		CreateConstantBuffer(g_Device.Get(), m_pScopeEffectBuffer.GetAddressOf(), sizeof(ScopeEffectShaderData));
		CreateConstantBuffer(g_Device.Get(), m_pConstantBufferData.GetAddressOf(), sizeof(ConstBufferData));

		// 动态常量缓冲区（示例）
		D3D11_BUFFER_DESC outputDesc = {};
		outputDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		outputDesc.Usage = D3D11_USAGE_DYNAMIC;
		outputDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		outputDesc.ByteWidth = 48;
		HR(g_Device->CreateBuffer(&outputDesc, nullptr, &targetVertexConstBufferOutPut));
		outputDesc.ByteWidth = 384;
		HR(g_Device->CreateBuffer(&outputDesc, nullptr, &targetVertexConstBufferOutPut1p5));
		outputDesc.ByteWidth = 752;
		HR(g_Device->CreateBuffer(&outputDesc, nullptr, &targetVertexConstBufferOutPut1));

		// 顶点/索引缓冲区
		CreateVertexBuffer(g_Device.Get(), &gdc_pVertexBuffer, gdc_Vertices, ARRAYSIZE(gdc_Vertices));
		CreateIndexBuffer(g_Device.Get(), &gdc_pIndexBuffer, gdc_Indices, ARRAYSIZE(gdc_Indices));

		// 采样器状态
		D3D11_SAMPLER_DESC sampDesc = {};
		sampDesc.Filter = D3D11_FILTER_ANISOTROPIC;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.MaxAnisotropy = 16;
		sampDesc.MinLOD = 0;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
		HR(g_Device->CreateSamplerState(&sampDesc, m_pSamplerState.GetAddressOf()));

		CreateBlender();
		return true;
	}

	void CreateTextureAndViews(
		ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format,
		ID3D11Texture2D** texture, ID3D11RenderTargetView** rtv = nullptr, ID3D11ShaderResourceView** srv = nullptr)
	{
		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = width;
		texDesc.Height = height;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = format;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		HR(device->CreateTexture2D(&texDesc, nullptr, texture));

		if (rtv) {
			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
			rtvDesc.Format = format;
			rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			HR(device->CreateRenderTargetView(*texture, &rtvDesc, rtv));
		}

		if (srv) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			HR(device->CreateShaderResourceView(*texture, &srvDesc, srv));
		}
	}

	void D3D::OnResize()
	{
		if (!g_Swapchain.Get() || !g_Device.Get()) {
			return;
		}

		DXGI_SWAP_CHAIN_DESC swapChainDesc{};
		if (SUCCEEDED(g_Swapchain->GetDesc(&swapChainDesc))) {
			windowWidth = static_cast<int>(swapChainDesc.BufferDesc.Width);
			windowHeight = static_cast<int>(swapChainDesc.BufferDesc.Height);
		}
		if (windowWidth <= 0 || windowHeight <= 0) {
			const auto* renderWindow = RE::BSGraphics::GetCurrentRendererWindow();
			if (!renderWindow) {
				logger::error("Unable to determine render dimensions");
				return;
			}
			windowWidth = renderWindow->windowWidth;
			windowHeight = renderWindow->windowHeight;
		}

		//------------------------------
		// 1. 创建平面索引缓冲区
		//------------------------------
		CreateIndexBuffer(g_Device.Get(), plane_pIndexBuffer.GetAddressOf(), plane_Indices, ARRAYSIZE(plane_Indices));

		//------------------------------
		// 2. 创建深度模板缓冲区和视图
		//------------------------------
		// 深度模板纹理描述
		D3D11_TEXTURE2D_DESC depthStencilDesc = {};
		depthStencilDesc.Width = windowWidth;
		depthStencilDesc.Height = windowHeight;
		depthStencilDesc.MipLevels = 1;
		depthStencilDesc.ArraySize = 1;
		depthStencilDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		depthStencilDesc.SampleDesc.Count = 1;
		depthStencilDesc.Usage = D3D11_USAGE_DEFAULT;
		depthStencilDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

		// 创建深度模板纹理和视图
		HR(g_Device->CreateTexture2D(&depthStencilDesc, nullptr, m_pDepthStencilBuffer.GetAddressOf()));
		HR(g_Device->CreateDepthStencilView(m_pDepthStencilBuffer.Get(), nullptr, m_pDepthStencilView.GetAddressOf()));

		//------------------------------
		// 3. 创建主渲染目标纹理及视图
		//------------------------------
		CreateTextureAndViews(
			g_Device.Get(),
			windowHeight, windowHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
			mRTRenderTargetTexture.GetAddressOf(),
			mRTRenderTargetView.GetAddressOf(),
			mRTShaderResourceView.GetAddressOf());

		//------------------------------
		// 4. 创建目标纹理及SRV（仅绑定到着色器资源）
		//------------------------------

		// 创建目标纹理

		// 创建关联的SRV
	}

	void D3D::ReleaseSizeDependentResources()
	{
		// ResizeBuffers requires every reference to the swap-chain back buffer
		// to be released before the original call executes.
		m_pDepthStencilBuffer.Reset();
		m_pDepthStencilView.Reset();
		m_pRenderTargetView.Reset();
		mRTRenderTargetTexture.Reset();
		mRTRenderTargetView.Reset();
		mRTShaderResourceView.Reset();
		mCurRTTexture.Reset();
		rtTexture2D.Reset();
		mShaderResourceView.Reset();
		mScopeFadeSceneSRV.Reset();
		mScopeFadeSceneTexture.Reset();
		{
			std::scoped_lock lock(mAutomaticSTSReticleLayerMutex);
			mAutomaticSTSReticleLayerWhiteSRV.Reset();
			mAutomaticSTSReticleLayerWhiteRTV.Reset();
			mAutomaticSTSReticleLayerWhiteTexture.Reset();
			mAutomaticSTSReticleLayerSRV.Reset();
			mAutomaticSTSReticleLayerRTV.Reset();
			mAutomaticSTSReticleLayerTexture.Reset();
			mAutomaticSTSReticleLayerCompositeBlend.Reset();
			mAutomaticSTSReticleSuppressionSourceBlend.Reset();
			mAutomaticSTSReticleColorSuppressionBlend.Reset();
			mAutomaticSTSReticleSuppressionSourceWasNull = false;
			mAutomaticSTSReticleLayerReadOnlyDepthState.Reset();
			mAutomaticSTSReticleLayerReady = false;
			mAutomaticSTSReticleLayerCaptureGeneration = 0U;
			mAutomaticSTSReticleLayerGeneration = 0U;
			mAutomaticSTSReticleLayerResourceGeneration = 0U;
		}
		mBackBuffer.Reset();

		SAFE_RELEASE(tempRt[0]);
		SAFE_RELEASE(tempRt[1]);
		SAFE_RELEASE(tempSV);
		SAFE_RELEASE(targetVertexConstBufferOutPut);
		SAFE_RELEASE(targetVertexConstBufferOutPut1p5);
		SAFE_RELEASE(targetVertexConstBufferOutPut1);
		SAFE_RELEASE(gdc_pVertexBuffer);
		SAFE_RELEASE(gdc_pVertexLayout);
		SAFE_RELEASE(gdc_pIndexBuffer);
		mBackBufferIndex = UINT_MAX;
		bHasGetBackBuffer = false;
		bIsFirst = true;
	}

	void D3D::LoadAimTexture(const std::string& path)
	{
		D3DInstance->mTextDDS_SRV.Reset();
		if (path.empty()) {
			return;
		}

		std::wstring defaultPath = L"Data/Textures/MagnaScope/Empty.dds";
		const wchar_t* tempPath = GetWC(path.c_str());

		HRESULT result = CreateDDSTextureFromFile(
			g_Device.Get(),
			tempPath ? tempPath : defaultPath.c_str(),
			nullptr,
			D3DInstance->mTextDDS_SRV.ReleaseAndGetAddressOf());
		if (FAILED(result) && tempPath) {
			logger::warn("Reticle texture {} failed to load; using Empty.dds", path);
			result = CreateDDSTextureFromFile(
				g_Device.Get(),
				defaultPath.c_str(),
				nullptr,
				D3DInstance->mTextDDS_SRV.ReleaseAndGetAddressOf());
		}
		if (FAILED(result)) {
			logger::error("No reticle texture could be loaded (HRESULT 0x{:08X})", static_cast<std::uint32_t>(result));
		}

		if (tempPath)
			free((void*)tempPath);
	}

	// 通用缓冲区更新模板
	template <typename T>
	void D3D::UpdateConstantBuffer(const ComPtr<ID3D11Buffer>& buffer, const T& data)
	{
		D3D11_MAPPED_SUBRESOURCE mapped;
		HR(g_Context->Map(buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		memcpy_s(mapped.pData, sizeof(T), &data, sizeof(T));
		g_Context->Unmap(buffer.Get(), 0);
	}

	void D3D::UpdateGameConstants(const GameConstBuffer& src, ScopeEffectShaderData& dst)
	{
		using namespace detail;
		// A later explicit assignment repairs the incomplete legacy matrix
		// copy below without changing the serialized constant-buffer layout.
		// 矩阵内存直接复制
		memcpy_s(&dst.CameraRotation, sizeof(float[16]), &src.camMat.entry[0], sizeof(float[9]));  // 只复制3x3部分，之前是float16的，注意看看有没有错误

		// Populate all matrix lanes explicitly. The preceding 36-byte legacy
		// copy crosses row boundaries and leaves the 4x4 tail undefined.
		dst.CameraRotation = {
			src.camMat.entry[0][0], src.camMat.entry[0][1], src.camMat.entry[0][2], 0.0F,
			src.camMat.entry[1][0], src.camMat.entry[1][1], src.camMat.entry[1][2], 0.0F,
			src.camMat.entry[2][0], src.camMat.entry[2][1], src.camMat.entry[2][2], 0.0F,
			0.0F, 0.0F, 0.0F, 1.0F
		};

		CopyVector3(dst.CurrRootPos, src.rootPos);
		CopyVector3(dst.CurrWeaponPos, src.weaponPos);
		CopyVector3(dst.eyeDirection, src.virDir);
		CopyVector3(dst.eyeDirectionLerp, src.VirDirLerp);
		CopyVector3(dst.eyeTranslationLerp, src.VirTransLerp);

		// 二维坐标
		dst.ScopeScreenPos = { src.scopeScreenPos.x, src.scopeScreenPos.y };
	}

	void D3D::UpdateScene(ScopeProfile* currData)
	{
		if (bChangeAimTexture) {
			LoadAimTexture(currData->ZoomNodePath);
			bChangeAimTexture = false;
		}

#pragma region FO4GameConstantBuffer

		scopeData.EnableMerge = isEnableScopeEffect ? 1 : 0;
		if (!isEnableScopeEffect) {
			scopeData.ScopeEffect_Zoom = currData->shaderData.minZoom;
		}

		UpdateGameConstants(gameConstBuffer, scopeData);
		scopeData.targetAdjustFov = pcam->fovAdjustCurrent;

		// In edit mode the menu's unsaved slider values bound the zoom, so the
		// magnification controls preview live instead of waiting for a save.
		const bool editing =
			bEnableEditMode.load(std::memory_order_acquire);
		const float zoomMin =
			editing ? editZoomMin : currData->shaderData.minZoom;
		const float zoomMax = std::max(
			zoomMin,
			editing ? editZoomMax : currData->shaderData.maxZoom);

		if (bResetZoomDelta) {
			gameZoomDelta = zoomMin;
			bResetZoomDelta = false;
		}

		gameZoomDelta = std::clamp(gameZoomDelta, zoomMin, zoomMax);

		scopeData.ScopeEffect_Zoom = gameZoomDelta;
		scopeData.GameFov = pcam->firstPersonFOV;

#pragma endregion

#pragma region MyScopeShaderData
		if (!editing) {
			const auto& shaderData = currData->shaderData;
			legacyFlag = currData->legacyMode;
			bLegacyMode = currData->legacyMode;

			// 二维参数处理
			const auto ToFloat2 = [](const auto& arr) { return XMFLOAT2(arr[0], arr[1]); };
			const auto ToFloat4 = [](const auto& arr) { return XMFLOAT4(arr[0], arr[1], arr[2], arr[3]); };

			// 基本参数映射
			scopeData.reticle_Offset = ToFloat2(shaderData.reticle_Offset);
			scopeData.BaseWeaponPos = shaderData.baseWeaponPos;
			scopeData.camDepth = shaderData.camDepth;
			scopeData.EnableNV = shaderData.bCanEnableNV ? bEnableNVG : 0;
			scopeData.EnableZMove = shaderData.bEnableZMove;
			scopeData.isCircle = shaderData.IsCircle;
			scopeData.MovePercentage = shaderData.movePercentage;
			scopeData.nvIntensity = shaderData.nvIntensity;
			scopeData.ReticleSize = shaderData.ReticleSize;
			scopeData.ScopeEffect_Offset = ToFloat2(LFA(shaderData.PositionOffset, 2));
			scopeData.ScopeEffect_OriPositionOffset = ToFloat2(LFA(shaderData.OriPositionOffset, 2));
			scopeData.ScopeEffect_OriSize = ToFloat2(shaderData.OriSize);
			scopeData.ScopeEffect_Size = ToFloat2(LFA(shaderData.Size, 2));
			scopeData.rect = ToFloat4(LFA(shaderData.rectSize, 4));

			// 视差参数
			const auto& parallax = shaderData.parallax;
			scopeData.parallax_maxTravel = parallax.maxTravel;
			scopeData.parallax_Radius = parallax.radius;
			scopeData.parallax_relativeFogRadius = parallax.relativeFogRadius;
			scopeData.parallax_scopeSwayAmount = parallax.scopeSwayAmount;
			scopeData.baseFovAdjustTarget = shaderData.fovAdjust;

			scopeData.FishEyeStrength = shaderData.fishEyeStrength;
			scopeData.FishEyePower = shaderData.fishEyePower;
		}
		scopeData.scopeDepth = {
			std::clamp(
				scopeSceneDepth.load(std::memory_order_acquire),
				0.0F,
				4.0F),
			std::clamp(
				scopeShadowDepth.load(std::memory_order_acquire),
				0.0F,
				4.0F)
		};
		scopeData.scopeDepthSeparation = {
			std::clamp(
				scopeImageStillness.load(std::memory_order_acquire),
				0.0F,
				1.0F),
			std::clamp(
				scopeAxialBreathing.load(std::memory_order_acquire),
				0.0F,
				4.0F),
			std::clamp(
				scopeApertureScaleRatio.load(std::memory_order_acquire),
				0.25F,
				4.0F),
			std::clamp(
				scopeTubeDepth.load(std::memory_order_acquire),
				0.0F,
				1.0F)
		};
#pragma endregion

		// 区域5: 统一更新常量缓冲区
		UpdateConstantBuffer(m_pScopeEffectBuffer, scopeData);

		// 特殊常量缓冲区
		constBufferData.width = windowWidth;
		constBufferData.height = windowHeight;
		UpdateConstantBuffer(m_pConstantBufferData, constBufferData);
	}

	void D3D::ScreenTextureMod()
	{
		UINT stride = sizeof(::Vertex);
		UINT offset = 0;
		const std::vector<VSConstantBufferSlot> noVertexConstants;
		SetupCommonRenderState(
			m_pVertexShader.Get(),
			nullptr,
			0,
			m_pPixelShader.Get(),
			gdc_pVertexLayout,
			BSTransparent.Get(),
			noVertexConstants,
			gdc_pIndexBuffer,
			DXGI_FORMAT_R32_UINT,
			0,
			&gdc_pVertexBuffer,
			&stride,
			&offset,
			1,
			mRTRenderTargetView.Get());

		// 指定图元类型为三角形列表
		g_Context->PSSetShaderResources(4, 1, D3DInstance->mShaderResourceView.GetAddressOf());
		g_Context->PSSetShaderResources(5, 1, D3DInstance->mTextDDS_SRV.GetAddressOf());

		bSelfDraw = true;
		g_Context->DrawIndexed(3, 0, 0);
		bSelfDraw = false;

		g_Context->OMSetRenderTargets(2, tempRt, tempSV);
	}

	// 完整的公共渲染状态设置
	void D3D::SetupCommonRenderState(
		ID3D11VertexShader* vs, ID3D11ClassInstance* const* vsClassInstances, UINT vsClassInstancesCount,
		ID3D11PixelShader* ps, ID3D11InputLayout* inputLayout, ID3D11BlendState* blendState, const std::vector<VSConstantBufferSlot>& vsCBSlots,
		ID3D11Buffer* indexBuffer, DXGI_FORMAT indexFormat, UINT indexOffset,
		ID3D11Buffer* const* vertexBuffers, const UINT* strides, const UINT* offsets, UINT numVertexBuffers, ID3D11RenderTargetView* backBufferRTV = nullptr)
	{
		// === 1. 深度模板状态配置 ===
		D3D11_DEPTH_STENCIL_DESC tempDSD{};
		D3D11_DEPTH_STENCILOP_DESC tempDSOPD{};
		tempDSOPD.StencilFailOp = D3D11_STENCIL_OP_KEEP;
		tempDSOPD.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
		tempDSOPD.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		tempDSOPD.StencilFunc = D3D11_COMPARISON_ALWAYS;

		tempDSD.DepthEnable = false;
		tempDSD.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		tempDSD.DepthFunc = D3D11_COMPARISON_LESS;
		tempDSD.StencilEnable = false;
		tempDSD.StencilReadMask = 255;
		tempDSD.StencilWriteMask = 255;
		tempDSD.FrontFace = tempDSOPD;
		tempDSD.BackFace = tempDSOPD;

		Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depthStencilState;
		HR(g_Device->CreateDepthStencilState(&tempDSD, depthStencilState.GetAddressOf()));

		// === 2. 核心渲染状态设置 ===
		// An explicit target (the swap chain back buffer at the Present
		// anchor) wins; otherwise composite into the game's bound target,
		// which is only correct mid-pipeline (TAA anchor).
		ID3D11RenderTargetView* rtvs[1] = {};
		rtvs[0] = backBufferRTV ? backBufferRTV :
		                          (upscalerMod ? tempRt[0] : (tempRt[1] ? tempRt[1] : tempRt[0]));
		g_Context->OMSetRenderTargets(1, rtvs, backBufferRTV ? nullptr : tempSV);

		// The fullscreen pass must cover the whole destination; the viewport
		// the game left bound (especially at Present time, after UI work) can
		// be a sub-rectangle. ScopedContextState restores the original.
		if (rtvs[0]) {
			Microsoft::WRL::ComPtr<ID3D11Resource> targetResource;
			rtvs[0]->GetResource(targetResource.GetAddressOf());
			Microsoft::WRL::ComPtr<ID3D11Texture2D> targetTexture;
			if (targetResource && SUCCEEDED(targetResource.As(&targetTexture))) {
				D3D11_TEXTURE2D_DESC targetDesc{};
				targetTexture->GetDesc(&targetDesc);
				D3D11_VIEWPORT viewport{};
				viewport.Width = static_cast<float>(targetDesc.Width);
				viewport.Height = static_cast<float>(targetDesc.Height);
				viewport.MaxDepth = 1.0F;
				g_Context->RSSetViewports(1, &viewport);
			}
		}

		// Never inherit the game's rasterizer state. At Present time the last
		// pass is Scaleform UI, which leaves scissor testing enabled with a
		// stale UI rectangle; that clips a fullscreen triangle down to
		// nothing even though the draw call itself succeeds. An explicit
		// state with scissor off makes the pass anchor-independent.
		// ScopedContextState restores the game's state afterwards.
		D3D11_RASTERIZER_DESC rasterDesc{};
		rasterDesc.FillMode = D3D11_FILL_SOLID;
		rasterDesc.CullMode = D3D11_CULL_NONE;
		rasterDesc.DepthClipEnable = TRUE;
		rasterDesc.ScissorEnable = FALSE;
		Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterState;
		if (SUCCEEDED(g_Device->CreateRasterizerState(&rasterDesc, rasterState.GetAddressOf()))) {
			g_Context->RSSetState(rasterState.Get());
		}
		// 复制资源（关键修复点）
		// 设置着色器
		g_Context->VSSetShader(vs, vsClassInstances, vsClassInstancesCount);  // 修复类实例传递
		g_Context->PSSetShader(ps, nullptr, 0);

		// 设置公共常量缓冲区
		g_Context->PSSetConstantBuffers(4, 1, m_pConstantBufferData.GetAddressOf());
		g_Context->PSSetConstantBuffers(5, 1, m_pScopeEffectBuffer.GetAddressOf());

		// === 3. 顶点相关设置 ===
		// 设置输入布局
		g_Context->IASetInputLayout(inputLayout);

		// 设置顶点缓冲区（修复指针传递）
		g_Context->IASetVertexBuffers(0, numVertexBuffers, vertexBuffers, strides, offsets);

		// 设置索引缓冲区
		g_Context->IASetIndexBuffer(indexBuffer, indexFormat, indexOffset);

		// === 4. 混合和深度状态 ===
		g_Context->OMSetBlendState(blendState, nullptr, 0xFFFFFFFF);
		g_Context->OMSetDepthStencilState(depthStencilState.Get(), 0);

		// === 5. 着色器资源 ===
		g_Context->PSSetSamplers(0, 1, m_pSamplerState.GetAddressOf());

		// === 6. 顶点着色器常量缓冲区 ===
		for (const auto& slot : vsCBSlots) {
			g_Context->VSSetConstantBuffers(slot.StartSlot, slot.NumBuffers, slot.ppBuffers);
		}

		// === 7. 图元拓扑 ===
		g_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	}

	bool D3D::RenderToReticleTexture()
	{
		if (!bIsFirst && isEnableRender) {
			const auto* currentProfile =
				ScopeData::ScopeDataHandler::GetSingleton()->GetCurrentScopeProfile();
			if (currentProfile &&
				currentProfile->autoProfile &&
				MagnaScope::GetSettings().AllowsGeometryMagnification()) {
				const bool exactScopeFadeReady =
					automaticSTSGeometryReady.load(std::memory_order_acquire) &&
					automaticSTSExactScopeFadeReplacementThisFrame.load(
						std::memory_order_acquire);
				if (!exactScopeFadeReady) {
					// Automatic STS magnification owns only the exact,
					// published ScopeFade draw. Falling through to the legacy
					// fullscreen circle would detach the effect from authored
					// lens geometry and can cover weapon or world pixels that
					// are outside the optic. Keep the ordinary STS draw when
					// exact capture was unavailable.
					return false;
				}

				// Replay the exact aperture against the composite target's own
				// content rather than the retired pre-first-person RT4
				// snapshot.
				//
				// That snapshot is taken at the RenderBatches boundary, which is
				// before Fallout's image-space and tone-mapping work. The
				// composite runs at the TAA or Present anchor, whose target is
				// the finished display-encoded frame. Sampling one into the
				// other applied no conversion at all, so the complete optical
				// image was uniformly darker than the surrounding scene no
				// matter what the shadow, magnification or cleanup controls were
				// set to. Runtime telemetry showed it directly: with the
				// composite target at format 28 (R8G8B8A8_UNORM, Present
				// anchor), the world source probed 23/29/33 at the aperture
				// centre while the same pixel of the displayed frame was bright.
				//
				// Passing no preferred source makes PrepareScopeFadeSceneSource
				// copy the composite target itself into the private coherent
				// texture, so source and destination are guaranteed to share one
				// colour encoding. See Through Scopes has already drawn the
				// world through the authored aperture by this point, so the
				// region being magnified is the correct optical image. First
				// person geometry is deliberately no longer excluded.
				ID3D11RenderTargetView* const compositeTarget =
					renderedAtTAAThisFrame ?
						nullptr :
						m_pRenderTargetView.Get();
				const bool replayed = ReplayAutomaticSTSScopeFade(
					nullptr,
					compositeTarget);
				if (replayed) {
					const bool reticleComposited =
						CompositeAutomaticSTSReticleLayer(
							compositeTarget);
					static std::once_flag loggedLateScopeFadeReplay;
					std::call_once(loggedLateScopeFadeReplay, [] {
						logger::info(
							"Automatic STS replays exact ScopeFade geometry "
							"against the coherent late-frame color source; "
							"the authored reticle is isolated from the optical "
							"scene and composited afterward");
					});
					if (!reticleComposited) {
						static std::once_flag loggedMissingReticleLayer;
						std::call_once(loggedMissingReticleLayer, [] {
							logger::warn(
								"Automatic STS optical replay completed without "
								"an independent reticle layer; the authored draw "
								"was not suppressed unless capture succeeded");
						});
					}
					return true;
				}

				// Reticle color may already have been removed from the coherent
				// scene before this late ScopeFade replay is attempted. If replay
				// fails, restore the independently captured authored reticle anyway
				// so a transient aperture failure cannot make the sight disappear.
				if (!renderedAtTAAThisFrame) {
					// Present is the final authoritative anchor. If aperture
					// replay fails here, restore any color-suppressed reticle so
					// it cannot disappear. At an earlier TAA anchor retain the
					// layer for the Present retry instead of consuming it into an
					// intermediate target.
					CompositeAutomaticSTSReticleLayer(compositeTarget);
				}

				// Capture success transfers this frame's ScopeFade color
				// ownership to the late replay. A failure here leaves a clear
				// lens for one frame instead of sampling contaminated glass or
				// drawing a mismatched fullscreen circle.
				static std::once_flag loggedLateScopeFadeReplayFailure;
				std::call_once(loggedLateScopeFadeReplayFailure, [] {
					logger::warn(
							"Automatic STS late ScopeFade replay was unavailable "
							"after aperture capture; this frame remains clear");
				});
				return false;
			}
			// 旧版特定参数

			std::vector<VSConstantBufferSlot> vsCBuffersSlots = {
				{ 1, 1, &targetVertexConstBufferOutPut },
				{ 2, 1, &targetVertexConstBufferOutPut1p5 },
			};

			UINT strides = sizeof(::Vertex);
			UINT offsets = 0;

			// No staging copy is needed here: Render() already duplicated the
			// live render target into mCurRTTexture with a matching format.
			// The old m_DstTexture path relied on CopyResource into a fixed
			// R8G8B8A8_UNORM texture, which silently no-ops when the bound
			// render target uses any other format and left the scope black.
			//
			// At the Present anchor the composite must land in the swap chain
			// back buffer to be visible; at the TAA anchor the game's bound
			// target is still mid-pipeline and correct.
			ID3D11RenderTargetView* const compositeTarget =
				renderedAtTAAThisFrame ? nullptr : m_pRenderTargetView.Get();
			ID3D11PixelShader* const pixelShader =
				currentProfile && currentProfile->autoProfile ?
					m_pPixelShader_AutoSTS.Get() :
					m_pPixelShader_Legacy.Get();
			SetupCommonRenderState(m_pVertexShader_Legacy.Get(), nullptr, 0, pixelShader, gdc_pVertexLayout, BSTransparent.Get(),
				vsCBuffersSlots, gdc_pIndexBuffer, DXGI_FORMAT_R32_UINT, 0, &gdc_pVertexBuffer, &strides, &offsets, 1, compositeTarget);

			// 旧版特定资源
			g_Context->PSSetShaderResources(4, 1, D3DInstance->mShaderResourceView.GetAddressOf());
			g_Context->PSSetShaderResources(5, 1, D3DInstance->mTextDDS_SRV.GetAddressOf());

			bSelfDraw = true;
			g_Context->DrawIndexed(3, 0, 0);
			bSelfDraw = false;
			return true;
		}
		return false;
	}

	void D3D::RenderToReticleTextureNew(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
	{
		if (targetVS.Get() && targetVertexConstBufferOutPut) {
			std::vector<VSConstantBufferSlot> vsCBuffersSlots = {
				{ 1, 1, &targetVertexConstBufferOutPut },
				{ 2, 1, &targetVertexConstBufferOutPut1p5 },
				{ 12, 1, &targetVertexConstBufferOutPut1 }
			};

			// Same anchor rule as the legacy pass: Present-anchor composites
			// go to the swap chain back buffer.
			ID3D11RenderTargetView* const compositeTarget =
				renderedAtTAAThisFrame ? nullptr : m_pRenderTargetView.Get();
			SetupCommonRenderState(
				targetVS.Get(), nullptr, 0, m_outPutPixelShader.Get(), targetInputLayout.Get(),
				BSTransparent.Get(), vsCBuffersSlots, targetIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, targetIndexBufferOffset, targetVertexBuffer.GetAddressOf(), &targetVertexBufferStrides,
				&targetVertexBufferOffsets, 1, compositeTarget);

			// 新版特有资源绑定
			//g_Context->PSSetShaderResources(1, 1, &nullSRV);  // 显式清空未使用的槽位
			// The intermediate is windowHeight square. Copying it into the
			// full-screen m_DstTexture is invalid when the aspect ratio is not
			// 1:1, so bind its matching SRV directly.
			g_Context->PSSetShaderResources(
				6,
				1,
				mRTShaderResourceView.GetAddressOf());

			bSelfDraw = true;
			g_Context->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
			bSelfDraw = false;
		}
	}

	void D3D::MapScopeEffectBuffer(ScopeEffectShaderData data)
	{
		scopeData = data;
	}

	bool IsTargetDrawCall(const BufferInfo& vertexInfo, const BufferInfo& indexInfo, UINT indexCount)
	{
		return vertexInfo.stride == TARGET_STRIDE && indexCount == TARGET_INDEX_COUNT && indexInfo.desc.ByteWidth == TARGET_BUFFER_SIZE && vertexInfo.desc.ByteWidth == TARGET_BUFFER_SIZE;
	}

	bool IsTargetTexture(ID3D11ShaderResourceView* srv)
	{
		if (!srv)
			return false;

		Microsoft::WRL::ComPtr<ID3D11Resource> pResource;
		srv->GetResource(&pResource);

		Microsoft::WRL::ComPtr<ID3D11Texture2D> pTexture2D;
		if (FAILED(pResource.As(&pTexture2D)))
			return false;

		D3D11_TEXTURE2D_DESC texDesc;
		pTexture2D->GetDesc(&texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srv->GetDesc(&srvDesc);

		return texDesc.Width == TARGET_TEXTURE_WIDTH && texDesc.Height == TARGET_TEXTURE_WIDTH && texDesc.Format == TARGET_TEXTURE_FORMAT && srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D;
	}

	UINT GetVertexBuffersInfo(
		ID3D11DeviceContext* pContext,
		std::vector<BufferInfo>& outInfos,
		UINT maxSlotsToCheck = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT)
	{
		outInfos.clear();

		// 1. 直接尝试获取所有可能的槽位
		std::vector<ID3D11Buffer*> buffers(maxSlotsToCheck);
		std::vector<UINT> strides(maxSlotsToCheck);
		std::vector<UINT> offsets(maxSlotsToCheck);

		pContext->IAGetVertexBuffers(0, maxSlotsToCheck, buffers.data(), strides.data(), offsets.data());

		// 2. 计算实际绑定的缓冲区数量
		UINT actualCount = 0;
		for (UINT i = 0; i < maxSlotsToCheck; ++i) {
			if (buffers[i] != nullptr) {
				actualCount++;
			}
		}

		if (actualCount == 0)
			return 0;

		// 3. 填充输出结构
		outInfos.resize(actualCount);
		UINT validIndex = 0;
		for (UINT i = 0; i < maxSlotsToCheck && validIndex < actualCount; ++i) {
			if (buffers[i] != nullptr) {
				outInfos[validIndex].stride = strides[i];
				outInfos[validIndex].offset = offsets[i];
				buffers[i]->GetDesc(&outInfos[validIndex].desc);
				buffers[i]->Release();  // 释放获取的引用
				validIndex++;
			}
		}

		return actualCount;
	}

	bool GetIndexBufferInfo(ID3D11DeviceContext* pContext, BufferInfo& outInfo)
	{
		Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
		DXGI_FORMAT format;

		// 获取索引缓冲区
		pContext->IAGetIndexBuffer(&indexBuffer, &format, &outInfo.offset);

		if (!indexBuffer)
			return false;

		// 获取缓冲区描述
		indexBuffer->GetDesc(&outInfo.desc);

		// 将格式信息存入stride（因为索引缓冲区没有stride概念）
		outInfo.stride = (format == DXGI_FORMAT_R32_UINT) ? 4 : 2;

		return true;
	}

	void CopyConstantBuffers(ID3D11DeviceContext* pContext, ID3D11Buffer* srcBuffers[], ID3D11Buffer* dstBuffers[], size_t count)
	{
		for (size_t i = 0; i < count; ++i) {
			if (srcBuffers[i] && dstBuffers[i]) {
				pContext->CopyResource(dstBuffers[i], srcBuffers[i]);
			}
		}
	}

	void D3D::DrawIndexedDispatch(
		ID3D11DeviceContext* pContext,
		UINT IndexCount,
		UINT StartIndexLocation,
		INT BaseVertexLocation,
		D3D11DrawIndexedHook original)
	{
		if (!original || !pContext) {
			return;
		}
		if (bSelfDraw) {
			return original(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
		}
		if (MagnaScope::GetSettings().AllowsWorldColorCapture()) {
			(void)MagnaScope::WorldOnlyScopeRenderer::GetSingleton()
				.CaptureBeforeFirstPersonDraw(pContext);
		}
		const auto* activeProfile = ScopeData::ScopeDataHandler::GetSingleton()->GetCurrentScopeProfile();
		if (activeProfile && activeProfile->autoProfile) {
			const auto& verification = MagnaScope::GetSettings();
			// Every draw that gets this far, whether or not the readiness gate
			// below lets it through. ScopeFade is submitted at roughly 94% of
			// the way through a frame -- ordinal 4470 of 4750 in a healthy
			// session -- so a session that only ever counts 75 draws per frame
			// stops looking long before the aperture arrives, and that is
			// exactly what both failed sessions recorded.
			//
			// Comparing this against the gated count separates the two causes.
			// If this reads a full frame while the gated count reads 75, the
			// readiness gate is closing mid-frame and invalidation needs to
			// become frame-coherent. If both read 75, our detour is not being
			// called at all and the problem is the hook chain.
			automaticSTSObservedDrawsThisFrame.fetch_add(
				1U,
				std::memory_order_relaxed);
			// ScopeFade is a child of STS's ScopeAiming branch. The actual draw is therefore the authoritative visibility signal.
			// Do not additionally gate on gunState: Fallout can briefly report
			// a non-sighted firing state while ScopeAiming remains visible,
			// which made Stage 4b and 4c disappear during recoil.
			if (verification.AllowsScopeFadeGeometry() &&
				automaticSTSGeometryReady.load(std::memory_order_acquire) &&
				D3DInstance->m_pGeometryShader_STSGeometryFill.Get() &&
				((verification.AllowsGeometryProbe() &&
					 D3DInstance->m_pPixelShader_STSGeometryProbe.Get()) ||
					(verification.AllowsGeometryMagnification() &&
						D3DInstance->m_pPixelShader_STSGeometryMagnify.Get()))) {
				const auto drawOrdinal =
					automaticSTSDrawOrdinalThisFrame.fetch_add(
						1U,
						std::memory_order_relaxed) +
					1U;
				ComPtr<ID3D11Buffer> currentVertexBuffer;
				ComPtr<ID3D11Buffer> currentIndexBuffer;
				UINT currentStride = 0;
				UINT currentVertexOffset = 0;
				DXGI_FORMAT currentIndexFormat = DXGI_FORMAT_UNKNOWN;
				UINT currentIndexOffset = 0;
				pContext->IAGetVertexBuffers(
					0,
					1,
					currentVertexBuffer.GetAddressOf(),
					&currentStride,
					&currentVertexOffset);
				pContext->IAGetIndexBuffer(
					currentIndexBuffer.GetAddressOf(),
					&currentIndexFormat,
					&currentIndexOffset);

				const bool exactGeometryMatch =
					reinterpret_cast<std::uintptr_t>(
						currentVertexBuffer.Get()) ==
						automaticSTSVertexBuffer.load(
							std::memory_order_relaxed) &&
					reinterpret_cast<std::uintptr_t>(
						currentIndexBuffer.Get()) ==
						automaticSTSIndexBuffer.load(
							std::memory_order_relaxed) &&
					IndexCount ==
						automaticSTSIndexCount.load(
							std::memory_order_relaxed) &&
					currentStride ==
						automaticSTSVertexStride.load(
							std::memory_order_relaxed);
				const auto expectedVertexDataOffset =
					automaticSTSVertexDataOffset.load(
						std::memory_order_relaxed);
				const auto expectedIndexDataOffset =
					automaticSTSIndexDataOffset.load(
						std::memory_order_relaxed);
				const std::int64_t effectiveVertexOffset =
					static_cast<std::int64_t>(currentVertexOffset) +
					static_cast<std::int64_t>(BaseVertexLocation) *
						static_cast<std::int64_t>(currentStride);
				const bool knownIndexFormat =
					currentIndexFormat == DXGI_FORMAT_R16_UINT ||
					currentIndexFormat == DXGI_FORMAT_R32_UINT;
				const std::uint32_t indexElementSize =
					currentIndexFormat == DXGI_FORMAT_R32_UINT ? 4U : 2U;
				const std::uint64_t effectiveIndexOffset =
					static_cast<std::uint64_t>(currentIndexOffset) +
					static_cast<std::uint64_t>(StartIndexLocation) *
						indexElementSize;
				const bool exactSuballocationMatch =
					knownIndexFormat &&
					(currentVertexOffset == expectedVertexDataOffset ||
						effectiveVertexOffset ==
							static_cast<std::int64_t>(
								expectedVertexDataOffset)) &&
					(currentIndexOffset == expectedIndexDataOffset ||
						effectiveIndexOffset == expectedIndexDataOffset);
				// A failed geometry match is silent, and that makes two very
				// different faults look identical from the log: See Through
				// Scopes not drawing the aperture at all, versus drawing it
				// against buffers we no longer recognize. Only the
				// suballocation rejection below is reported, and it is never
				// reached when the buffer pointer, index count, or stride is
				// what diverged.
				//
				// Both have been observed to latch for an entire session and
				// clear on a game restart, which is the signature of the
				// published identity going stale rather than of a transient.
				//
				// Fallout pools unrelated meshes into the same buffers, so
				// "shares the pooled buffer pair" selects most of the world and
				// is useless as a filter -- a first attempt at this spent its
				// whole log budget on 195555-index terrain. The index count and
				// stride together are what actually make a draw ScopeFade
				// shaped, so count those per frame and report only those.
				if (!exactGeometryMatch) {
					const auto expectedVertexBuffer =
						automaticSTSVertexBuffer.load(
							std::memory_order_relaxed);
					const auto expectedIndexBuffer =
						automaticSTSIndexBuffer.load(
							std::memory_order_relaxed);
					const auto expectedIndexCount =
						automaticSTSIndexCount.load(
							std::memory_order_relaxed);
					const auto expectedStride =
						automaticSTSVertexStride.load(
							std::memory_order_relaxed);
					if (IndexCount == expectedIndexCount &&
						currentStride == expectedStride) {
						automaticSTSScopeFadeShapedDrawsThisFrame.fetch_add(
							1U,
							std::memory_order_relaxed);
						static std::atomic_uint32_t
							loggedGeometryMismatches{ 0U };
						if (loggedGeometryMismatches.fetch_add(
								1U,
								std::memory_order_relaxed) < 8U) {
							logger::info(
								"Stage 4d ScopeFade shaped draw rejected: "
								"VB=0x{:x} expected 0x{:x}, "
								"IB=0x{:x} expected 0x{:x}, "
								"vertexOffset={} effective={} expected={}, "
								"indexOffset={} effective={} expected={}, "
								"ordinal={}",
								reinterpret_cast<std::uintptr_t>(
									currentVertexBuffer.Get()),
								expectedVertexBuffer,
								reinterpret_cast<std::uintptr_t>(
									currentIndexBuffer.Get()),
								expectedIndexBuffer,
								currentVertexOffset,
								effectiveVertexOffset,
								expectedVertexDataOffset,
								currentIndexOffset,
								effectiveIndexOffset,
								expectedIndexDataOffset,
								drawOrdinal);
						}
					}
				}

				const bool exactReticleMatch =
					MatchesAutomaticSTSReticleSet(
						currentVertexBuffer.Get(),
						currentIndexBuffer.Get(),
						IndexCount,
						currentStride,
						currentVertexOffset,
						effectiveVertexOffset,
						currentIndexOffset,
						effectiveIndexOffset,
						knownIndexFormat);
				if (exactReticleMatch) {
					automaticSTSReticleDrawsThisFrame.fetch_add(
						1U,
						std::memory_order_relaxed);
					automaticSTSLastReticleOrdinal.store(
						drawOrdinal,
						std::memory_order_relaxed);
					// ScopeFade is submitted before Reticle:0 in the STS branch.
					// Only suppress the authored reticle after exact-aperture
					// capture has already succeeded for this frame. Otherwise the
					// ordinary STS draw remains the fail-safe.
					if (automaticSTSExactScopeFadeReplacementThisFrame.load(
							std::memory_order_acquire)) {
						auto reticleLayerCaptureLock =
							D3DInstance->LockAutomaticSTSReticleLayerCapture();
						ComPtr<ID3D11RenderTargetView> sourceTarget;
						ComPtr<ID3D11DepthStencilView> sourceDepth;
						pContext->OMGetRenderTargets(
							1U,
							sourceTarget.GetAddressOf(),
							sourceDepth.GetAddressOf());
						bool suppressionReady = false;
						{
							ScopedContextState restoreAfterSuppressionProbe(
								pContext);
							suppressionReady =
								D3DInstance->ApplyAutomaticSTSReticleColorSuppression(
									pContext);
						}
						bool capturedBlack = false;
						if (suppressionReady) {
							ScopedContextState restoreAfterBlackCapture(pContext);
							if (D3DInstance->BeginAutomaticSTSReticleLayerCapture(
									pContext,
									sourceTarget.Get(),
									sourceDepth.Get(),
									false)) {
								bSelfDraw = true;
								original(
									pContext,
									IndexCount,
									StartIndexLocation,
									BaseVertexLocation);
								bSelfDraw = false;
								capturedBlack = true;
							}
						}
						bool capturedWhite = false;
						if (capturedBlack) {
							// The black capture bound its private RTV. Restore the
							// authored OM state before asking the white capture to
							// validate RT0; sharing one RAII scope made this check
							// fail deterministically and left the reticle embedded in
							// the scene that receives magnification.
							ScopedContextState restoreAfterWhiteCapture(pContext);
							if (D3DInstance->BeginAutomaticSTSReticleLayerCapture(
									pContext,
									sourceTarget.Get(),
									sourceDepth.Get(),
									true)) {
								bSelfDraw = true;
								original(
									pContext,
									IndexCount,
									StartIndexLocation,
									BaseVertexLocation);
								bSelfDraw = false;
								capturedWhite = true;
							}
						}
						bool captured = capturedBlack && capturedWhite;
						if (captured) {
							// Preserve the authored draw's depth, stencil, and auxiliary
							// MRT effects exactly once while removing only displayed
							// color. This is what makes the reticle absent from the scene
							// image that receives ScopeEffect_Zoom.
							ScopedContextState restoreAfterSuppression(pContext);
							captured =
								D3DInstance->ApplyAutomaticSTSReticleColorSuppression(
									pContext);
							if (captured) {
								bSelfDraw = true;
								original(
									pContext,
									IndexCount,
									StartIndexLocation,
									BaseVertexLocation);
								bSelfDraw = false;
							}
						}
						D3DInstance->CompleteAutomaticSTSReticleLayerCapture(
							captured,
							automaticSTSReplayFrameGeneration.load(
								std::memory_order_acquire));
						if (captured) {
							return;
						}
					}
				}

				bool exactHousingMatch = false;
				if (automaticSTSHousingGeometryReady.load(
						std::memory_order_acquire)) {
					const auto expectedHousingVertexDataOffset =
						automaticSTSHousingVertexDataOffset.load(
							std::memory_order_relaxed);
					const auto expectedHousingIndexDataOffset =
						automaticSTSHousingIndexDataOffset.load(
							std::memory_order_relaxed);
					exactHousingMatch =
						reinterpret_cast<std::uintptr_t>(
							currentVertexBuffer.Get()) ==
							automaticSTSHousingVertexBuffer.load(
								std::memory_order_relaxed) &&
						reinterpret_cast<std::uintptr_t>(
							currentIndexBuffer.Get()) ==
							automaticSTSHousingIndexBuffer.load(
								std::memory_order_relaxed) &&
						IndexCount ==
							automaticSTSHousingIndexCount.load(
								std::memory_order_relaxed) &&
						currentStride ==
							automaticSTSHousingVertexStride.load(
								std::memory_order_relaxed) &&
						knownIndexFormat &&
						(currentVertexOffset ==
								expectedHousingVertexDataOffset ||
							effectiveVertexOffset ==
								static_cast<std::int64_t>(
									expectedHousingVertexDataOffset)) &&
						(currentIndexOffset ==
								expectedHousingIndexDataOffset ||
							effectiveIndexOffset ==
								expectedHousingIndexDataOffset);
				}
				if (exactHousingMatch) {
					automaticSTSHousingDrawsThisFrame.fetch_add(
						1U,
						std::memory_order_relaxed);
					automaticSTSLastHousingOrdinal.store(
						drawOrdinal,
						std::memory_order_relaxed);
				}

				if (exactGeometryMatch && !exactSuballocationMatch) {
					// FO4 pools many shapes into the same large D3D buffers.
					// A buffer-pointer and index-count match is therefore only
					// a candidate until its byte suballocation also matches.
					// Keep diagnostics bounded on the render thread. A no-match
					// is deliberately safer than recoloring unrelated geometry.
					static std::atomic_uint32_t loggedSuballocationMismatches{ 0 };
					const auto diagnosticIndex =
						loggedSuballocationMismatches.fetch_add(
							1,
							std::memory_order_relaxed);
					if (diagnosticIndex < 8U) {
						logger::info(
							"Stage 4d ScopeFade buffer candidate rejected: "
							"vertexOffset={} effective={} expected={}, "
							"indexOffset={} effective={} expected={}, "
							"indexFormat={}, startIndex={}, baseVertex={}",
							currentVertexOffset,
							effectiveVertexOffset,
							expectedVertexDataOffset,
							currentIndexOffset,
							effectiveIndexOffset,
							expectedIndexDataOffset,
							static_cast<int>(currentIndexFormat),
							StartIndexLocation,
							BaseVertexLocation);
					}
				}

				if (exactGeometryMatch && exactSuballocationMatch) {
					const bool magnificationStage =
						verification.AllowsGeometryMagnification();
					// The retired pre-first-person world snapshot used to gate
					// this branch: without a valid snapshot the aperture draw
					// was neither recorded nor suppressed. The optical source is
					// now the composite target's own content, which is always
					// available at replay time, so requiring that snapshot here
					// only suppressed the effect entirely -- the aperture was
					// never captured, the replay never armed, and the lens fell
					// through to ordinary See Through Scopes.

					// Resource rebinding and the replacement draw form one
					// transaction. This prevents a concurrent wrapped
					// DrawIndexed call from replacing device children while
					// this context still has them bound.
					std::scoped_lock geometryLock(
						D3DInstance->mScopeFadeGeometryMutex);
					automaticSTSScopeFadeDrawsThisFrame.fetch_add(
						1U,
						std::memory_order_relaxed);
					automaticSTSLastScopeFadeOrdinal.store(
						drawOrdinal,
						std::memory_order_relaxed);
					if (magnificationStage) {
						// MagnaScope does not magnify the partially
						// rendered target at the aperture's original draw.
						// It records the aperture draw, lets the full weapon
						// and world finish, then replays the aperture over a
						// coherent late color source. Preserve that contract
						// for automatic STS scopes.
						const bool captured =
							D3DInstance
								->CaptureAutomaticSTSScopeFadeReplay(
									pContext,
									IndexCount,
									StartIndexLocation,
									BaseVertexLocation,
									currentVertexBuffer.Get(),
									currentStride,
									currentVertexOffset,
									currentIndexBuffer.Get(),
									currentIndexFormat,
									currentIndexOffset);
						automaticSTSExactScopeFadeReplacementThisFrame.store(
							captured,
							std::memory_order_release);
						if (captured) {
							// Do not put authored glass into the late source.
							// It would be magnified and replayed recursively,
							// producing the stable projected-object and thin
							// rim artifacts seen above 1x.
							return original(
								pContext,
								0,
								0,
								0);
						}
						return original(
							pContext,
							IndexCount,
							StartIndexLocation,
							BaseVertexLocation);
					}

					// Record the authored occlusion contract once. The filled
					// center deliberately keeps ScopeFade's depth state; this
					// evidence distinguishes a true nearer-depth rejection
					// from foreground weapon geometry that never wrote to this
					// depth surface or lies beyond the optical plane.
					static std::once_flag loggedScopeFadeDepthContract;
					std::call_once(loggedScopeFadeDepthContract, [&] {
						ComPtr<ID3D11DepthStencilState> depthState;
						UINT stencilReference = 0;
						pContext->OMGetDepthStencilState(
							depthState.GetAddressOf(),
							&stencilReference);
						D3D11_DEPTH_STENCIL_DESC depthDescription{};
						if (depthState.Get()) {
							depthState->GetDesc(&depthDescription);
						}

						ComPtr<ID3D11RenderTargetView> diagnosticTarget;
						ComPtr<ID3D11DepthStencilView> depthView;
						pContext->OMGetRenderTargets(
							1,
							diagnosticTarget.GetAddressOf(),
							depthView.GetAddressOf());
						D3D11_DEPTH_STENCIL_VIEW_DESC viewDescription{};
						if (depthView.Get()) {
							depthView->GetDesc(&viewDescription);
						}

						logger::info(
							"ScopeFade authored depth contract: "
							"state={}, enabled={}, writeMask={}, func={}, "
							"stencilRef={}, DSV={}, format={}, dimension={}",
							depthState.Get() != nullptr,
							depthDescription.DepthEnable,
							static_cast<int>(
								depthDescription.DepthWriteMask),
							static_cast<int>(
								depthDescription.DepthFunc),
							stencilReference,
							depthView.Get() != nullptr,
							static_cast<int>(viewDescription.Format),
							static_cast<int>(
								viewDescription.ViewDimension));
					});

					// Preserve every stage touched by the ScopeFade
					// replacement. The existing vertex shader, transform
					// constants, input assembly, rasterizer, depth, blend,
					// render target, and draw order remain Fallout's authored
					// contract. Fallout's shaders on this path do not use
					// dynamic linkage, so query only the shader objects. Large
					// class-instance arrays are not safe through every context
					// wrapper used by frame-generation integrations.
					ComPtr<ID3D11PixelShader> originalPixelShader;
					pContext->PSGetShader(
						originalPixelShader.GetAddressOf(),
						nullptr,
						nullptr);

					ComPtr<ID3D11GeometryShader> originalGeometryShader;
					pContext->GSGetShader(
						originalGeometryShader.GetAddressOf(),
						nullptr,
						nullptr);

					pContext->GSSetShader(
						D3DInstance->m_pGeometryShader_STSGeometryFill.Get(),
						nullptr,
						0);
					pContext->PSSetShader(
						D3DInstance->m_pPixelShader_STSGeometryProbe.Get(),
						nullptr,
						0);
					original(
						pContext,
						IndexCount,
						StartIndexLocation,
						BaseVertexLocation);

					pContext->PSSetShader(
						originalPixelShader.Get(),
						nullptr,
						0);
					pContext->GSSetShader(
						originalGeometryShader.Get(),
						nullptr,
						0);

					static std::once_flag loggedGeometryDrawMatch;
					std::call_once(loggedGeometryDrawMatch, [&] {
						logger::info(
							"Exact ScopeFade draw matched for {}: "
							"VB={:p}, IB={:p}, indices={}, stride={}, "
							"vertexOffset={}/{}, indexOffset={}/{}, "
							"indexFormat={}, startIndex={}, baseVertex={}; "
							"the geometry shader filled the annulus and the "
							"pixel shader was replaced",
							"Stage 4d cyan probing",
							static_cast<void*>(
								currentVertexBuffer.Get()),
							static_cast<void*>(
								currentIndexBuffer.Get()),
							IndexCount,
							currentStride,
							currentVertexOffset,
							expectedVertexDataOffset,
							currentIndexOffset,
							expectedIndexDataOffset,
							static_cast<int>(currentIndexFormat),
							StartIndexLocation,
							BaseVertexLocation);
					});
					return;
				}
			}

			// STS owns its 3D reticle. Suppressing the inherited MagnaScope
			// fingerprinted draw here would make the reticle disappear in
			// automatic mode.
			return original(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
		}

		std::vector<BufferInfo> vertexInfo;
		BufferInfo indexInfo;
		if (!GetVertexBuffersInfo(pContext, vertexInfo) || !GetIndexBufferInfo(pContext, indexInfo)) {
			return original(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
		}

		if (IsTargetDrawCall(vertexInfo[0], indexInfo, IndexCount)) {
			pContext->VSGetShader(targetVS.ReleaseAndGetAddressOf(), nullptr, nullptr);
			pContext->PSGetShader(targetPS.ReleaseAndGetAddressOf(), nullptr, nullptr);
			pContext->OMGetDepthStencilState(targetDepthStencilState.ReleaseAndGetAddressOf(), nullptr);
			pContext->IAGetInputLayout(targetInputLayout.ReleaseAndGetAddressOf());

			pContext->IAGetIndexBuffer(targetIndexBuffer.ReleaseAndGetAddressOf(), &targetIndexBufferFormat, &targetIndexBufferOffset);

			pContext->IAGetVertexBuffers(0, 1, targetVertexBuffer.ReleaseAndGetAddressOf(), &targetVertexBufferStrides, &targetVertexBufferOffsets);
			SAFE_RELEASE(targetVertexConstBuffer);
			SAFE_RELEASE(targetVertexConstBuffer1p5);
			SAFE_RELEASE(targetVertexConstBuffer1);
			pContext->VSGetConstantBuffers(1, 1, &targetVertexConstBuffer);
			pContext->VSGetConstantBuffers(2, 1, &targetVertexConstBuffer1p5);
			pContext->VSGetConstantBuffers(12, 1, &targetVertexConstBuffer1);

			pContext->PSGetShaderResources(0, 1, DrawIndexedSRV.ReleaseAndGetAddressOf());

			if (!DrawIndexedSRV.Get() || !oldFuncs.phookD3D11DrawIndexed) {
				original(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
				return;
			}

			ComPtr<ID3D11Resource> pResource;
			DrawIndexedSRV->GetResource(pResource.GetAddressOf());
			if (!pResource.Get()) {
				return original(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
			}
			D3D11_SHADER_RESOURCE_VIEW_DESC tempSRVDesc;
			DrawIndexedSRV->GetDesc(&tempSRVDesc);

			// 获取资源的类型
			D3D11_RESOURCE_DIMENSION dimension;
			pResource->GetType(&dimension);

			// 检查是否是 2D 纹理
			if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
				// 使用 QueryInterface 获取 ID3D11Texture2D 接口的指针
				ComPtr<ID3D11Texture2D> pTexture2D;
				HRESULT hr = pResource.As(&pTexture2D);
				if (SUCCEEDED(hr)) {
					// 获取纹理描述
					D3D11_TEXTURE2D_DESC desc;
					pTexture2D->GetDesc(&desc);

					if (desc.Width == TARGET_TEXTURE_WIDTH && desc.Height == TARGET_TEXTURE_WIDTH && desc.ArraySize == 1 && desc.Format == TARGET_TEXTURE_FORMAT && tempSRVDesc.Format == TARGET_TEXTURE_FORMAT && tempSRVDesc.Texture2D.MipLevels == 1 && tempSRVDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
						if (targetVertexConstBuffer && targetVertexConstBuffer1p5 && targetVertexConstBuffer1) {
							ID3D11Buffer* srcbuffers[] = { targetVertexConstBuffer, targetVertexConstBuffer1p5, targetVertexConstBuffer1 };
							ID3D11Buffer* dstBuffers[] = { targetVertexConstBufferOutPut, targetVertexConstBufferOutPut1p5, targetVertexConstBufferOutPut1 };
							CopyConstantBuffers(pContext, srcbuffers, dstBuffers, 3);
						}

						targetIndexCount = IndexCount;
						targetStartIndexLocation = StartIndexLocation;
						targetBaseVertexLocation = BaseVertexLocation;

						return original(pContext, 0, 0, 0);
					}
				}
			}
		}

		return original(pContext, IndexCount, StartIndexLocation, BaseVertexLocation);
	}

	// Two detours per draw entry, one per hooked target. MinHook hands each
	// target its own trampoline and a shared detour has no way to tell which
	// one it was entered through, so the pairing has to be static.
	void __stdcall D3D::DrawIndexedHook(
		ID3D11DeviceContext* pContext,
		UINT IndexCount,
		UINT StartIndexLocation,
		INT BaseVertexLocation)
	{
		DrawIndexedDispatch(
			pContext,
			IndexCount,
			StartIndexLocation,
			BaseVertexLocation,
			oldFuncs.phookD3D11DrawIndexed);
	}

	void __stdcall D3D::DrawIndexedHookAlternate(
		ID3D11DeviceContext* pContext,
		UINT IndexCount,
		UINT StartIndexLocation,
		INT BaseVertexLocation)
	{
		DrawIndexedDispatch(
			pContext,
			IndexCount,
			StartIndexLocation,
			BaseVertexLocation,
			oldFuncs.phookD3D11DrawIndexedAlternate);
	}

	void D3D::DrawIndexedInstancedDispatch(
		ID3D11DeviceContext* pContext,
		UINT IndexCountPerInstance,
		UINT InstanceCount,
		UINT StartIndexLocation,
		INT BaseVertexLocation,
		UINT StartInstanceLocation,
		D3D11DrawIndexedInstancedHook original)
	{
		if (!original) {
			return;
		}
		if (!pContext) {
			return;
		}
		const auto callOriginal = [&] {
			original(
				pContext,
				IndexCountPerInstance,
				InstanceCount,
				StartIndexLocation,
				BaseVertexLocation,
				StartInstanceLocation);
		};
		if (bSelfDraw) {
			callOriginal();
			return;
		}
		if (MagnaScope::GetSettings().AllowsWorldColorCapture()) {
			(void)MagnaScope::WorldOnlyScopeRenderer::GetSingleton()
				.CaptureBeforeFirstPersonDraw(pContext);
		}
		const auto* activeProfile =
			ScopeData::ScopeDataHandler::GetSingleton()->GetCurrentScopeProfile();
		const auto& verification = MagnaScope::GetSettings();
		// Counted separately from the non-instanced hook, and before the
		// readiness gate, because the two hooks are separate detours that can
		// fail independently. They share one draw ordinal, so a single total
		// cannot say which of them went quiet.
		if (activeProfile && activeProfile->autoProfile) {
			automaticSTSObservedInstancedDrawsThisFrame.fetch_add(
				1U,
				std::memory_order_relaxed);
		}
		if (!activeProfile || !activeProfile->autoProfile ||
			!verification.AllowsScopeFadeGeometry() ||
			!automaticSTSGeometryReady.load(std::memory_order_acquire)) {
			callOriginal();
			return;
		}

		const auto drawOrdinal =
			automaticSTSDrawOrdinalThisFrame.fetch_add(
				1U,
				std::memory_order_relaxed) +
			1U;
		ComPtr<ID3D11Buffer> currentVertexBuffer;
		ComPtr<ID3D11Buffer> currentIndexBuffer;
		UINT currentStride = 0;
		UINT currentVertexOffset = 0;
		DXGI_FORMAT currentIndexFormat = DXGI_FORMAT_UNKNOWN;
		UINT currentIndexOffset = 0;
		pContext->IAGetVertexBuffers(
			0,
			1,
			currentVertexBuffer.GetAddressOf(),
			&currentStride,
			&currentVertexOffset);
		pContext->IAGetIndexBuffer(
			currentIndexBuffer.GetAddressOf(),
			&currentIndexFormat,
			&currentIndexOffset);

		const bool knownIndexFormat =
			currentIndexFormat == DXGI_FORMAT_R16_UINT ||
			currentIndexFormat == DXGI_FORMAT_R32_UINT;
		const std::uint32_t indexElementSize =
			currentIndexFormat == DXGI_FORMAT_R32_UINT ? 4U : 2U;
		const std::int64_t effectiveVertexOffset =
			static_cast<std::int64_t>(currentVertexOffset) +
			static_cast<std::int64_t>(BaseVertexLocation) *
				static_cast<std::int64_t>(currentStride);
		const std::uint64_t effectiveIndexOffset =
			static_cast<std::uint64_t>(currentIndexOffset) +
			static_cast<std::uint64_t>(StartIndexLocation) *
				indexElementSize;

		const auto matchesPublished = [&](
										  const std::atomic<std::uintptr_t>& publishedVertexBuffer,
										  const std::atomic<std::uintptr_t>& publishedIndexBuffer,
										  const std::atomic_uint32_t& publishedIndexCount,
										  const std::atomic_uint32_t& publishedVertexStride,
										  const std::atomic_uint32_t& publishedVertexDataOffset,
										  const std::atomic_uint32_t& publishedIndexDataOffset,
										  const std::atomic_bool& publishedReady) {
			if (!publishedReady.load(std::memory_order_acquire) ||
				!knownIndexFormat) {
				return false;
			}
			const auto expectedVertexDataOffset =
				publishedVertexDataOffset.load(
					std::memory_order_relaxed);
			const auto expectedIndexDataOffset =
				publishedIndexDataOffset.load(
					std::memory_order_relaxed);
			return reinterpret_cast<std::uintptr_t>(
					   currentVertexBuffer.Get()) ==
			           publishedVertexBuffer.load(
						   std::memory_order_relaxed) &&
			       reinterpret_cast<std::uintptr_t>(
					   currentIndexBuffer.Get()) ==
			           publishedIndexBuffer.load(
						   std::memory_order_relaxed) &&
			       IndexCountPerInstance ==
			           publishedIndexCount.load(
						   std::memory_order_relaxed) &&
			       currentStride ==
			           publishedVertexStride.load(
						   std::memory_order_relaxed) &&
			       (currentVertexOffset == expectedVertexDataOffset ||
					   effectiveVertexOffset ==
						   static_cast<std::int64_t>(
							   expectedVertexDataOffset)) &&
			       (currentIndexOffset == expectedIndexDataOffset ||
					   effectiveIndexOffset == expectedIndexDataOffset);
		};

		if (matchesPublished(
				automaticSTSVertexBuffer,
				automaticSTSIndexBuffer,
				automaticSTSIndexCount,
				automaticSTSVertexStride,
				automaticSTSVertexDataOffset,
				automaticSTSIndexDataOffset,
				automaticSTSGeometryReady)) {
			automaticSTSScopeFadeInstancedDrawsThisFrame.fetch_add(
				1U,
				std::memory_order_relaxed);
			automaticSTSLastScopeFadeOrdinal.store(
				drawOrdinal,
				std::memory_order_relaxed);
			static std::once_flag loggedInstancedScopeFadeFallback;
			std::call_once(loggedInstancedScopeFadeFallback, [] {
				logger::info(
					"Exact ScopeFade was submitted through "
					"DrawIndexedInstanced; that authored draw remains "
					"untouched and the fullscreen AutoSTS pass stays enabled");
			});
		}
		const bool exactReticleMatch = MatchesAutomaticSTSReticleSet(
			currentVertexBuffer.Get(),
			currentIndexBuffer.Get(),
			IndexCountPerInstance,
			currentStride,
			currentVertexOffset,
			effectiveVertexOffset,
			currentIndexOffset,
			effectiveIndexOffset,
			knownIndexFormat);
		if (exactReticleMatch) {
			automaticSTSReticleInstancedDrawsThisFrame.fetch_add(
				1U,
				std::memory_order_relaxed);
			automaticSTSLastReticleOrdinal.store(
				drawOrdinal,
				std::memory_order_relaxed);
			if (automaticSTSExactScopeFadeReplacementThisFrame.load(
					std::memory_order_acquire)) {
				auto reticleLayerCaptureLock =
					D3DInstance->LockAutomaticSTSReticleLayerCapture();
				ComPtr<ID3D11RenderTargetView> sourceTarget;
				ComPtr<ID3D11DepthStencilView> sourceDepth;
				pContext->OMGetRenderTargets(
					1U,
					sourceTarget.GetAddressOf(),
					sourceDepth.GetAddressOf());
			bool suppressionReady = false;
			{
				ScopedContextState restoreAfterSuppressionProbe(pContext);
				suppressionReady =
					D3DInstance->ApplyAutomaticSTSReticleColorSuppression(
						pContext);
			}
			bool capturedBlack = false;
			if (suppressionReady) {
				ScopedContextState restoreAfterBlackCapture(pContext);
				if (D3DInstance->BeginAutomaticSTSReticleLayerCapture(
							pContext,
							sourceTarget.Get(),
							sourceDepth.Get(),
							false)) {
						bSelfDraw = true;
						original(
							pContext,
							IndexCountPerInstance,
							InstanceCount,
							StartIndexLocation,
							BaseVertexLocation,
							StartInstanceLocation);
						bSelfDraw = false;
					capturedBlack = true;
				}
			}
			bool capturedWhite = false;
			if (capturedBlack) {
				ScopedContextState restoreAfterWhiteCapture(pContext);
				if (D3DInstance->BeginAutomaticSTSReticleLayerCapture(
							pContext,
							sourceTarget.Get(),
							sourceDepth.Get(),
							true)) {
						bSelfDraw = true;
						original(
							pContext,
							IndexCountPerInstance,
							InstanceCount,
							StartIndexLocation,
							BaseVertexLocation,
							StartInstanceLocation);
						bSelfDraw = false;
					capturedWhite = true;
				}
			}
				bool captured = capturedBlack && capturedWhite;
				if (captured) {
					ScopedContextState restoreAfterSuppression(pContext);
					captured =
						D3DInstance->ApplyAutomaticSTSReticleColorSuppression(
							pContext);
					if (captured) {
						bSelfDraw = true;
						original(
							pContext,
							IndexCountPerInstance,
							InstanceCount,
							StartIndexLocation,
							BaseVertexLocation,
							StartInstanceLocation);
						bSelfDraw = false;
					}
				}
				D3DInstance->CompleteAutomaticSTSReticleLayerCapture(
					captured,
					automaticSTSReplayFrameGeneration.load(
						std::memory_order_acquire));
				if (captured) {
					return;
				}
			}
		}
		if (matchesPublished(
				automaticSTSHousingVertexBuffer,
				automaticSTSHousingIndexBuffer,
				automaticSTSHousingIndexCount,
				automaticSTSHousingVertexStride,
				automaticSTSHousingVertexDataOffset,
				automaticSTSHousingIndexDataOffset,
				automaticSTSHousingGeometryReady)) {
			automaticSTSHousingInstancedDrawsThisFrame.fetch_add(
				1U,
				std::memory_order_relaxed);
			automaticSTSLastHousingOrdinal.store(
				drawOrdinal,
				std::memory_order_relaxed);
		}

		callOriginal();
	}

	void __stdcall D3D::DrawIndexedInstancedHook(
		ID3D11DeviceContext* pContext,
		UINT IndexCountPerInstance,
		UINT InstanceCount,
		UINT StartIndexLocation,
		INT BaseVertexLocation,
		UINT StartInstanceLocation)
	{
		DrawIndexedInstancedDispatch(
			pContext,
			IndexCountPerInstance,
			InstanceCount,
			StartIndexLocation,
			BaseVertexLocation,
			StartInstanceLocation,
			oldFuncs.phookD3D11DrawIndexedInstanced);
	}

	void __stdcall D3D::DrawIndexedInstancedHookAlternate(
		ID3D11DeviceContext* pContext,
		UINT IndexCountPerInstance,
		UINT InstanceCount,
		UINT StartIndexLocation,
		INT BaseVertexLocation,
		UINT StartInstanceLocation)
	{
		DrawIndexedInstancedDispatch(
			pContext,
			IndexCountPerInstance,
			InstanceCount,
			StartIndexLocation,
			BaseVertexLocation,
			StartInstanceLocation,
			oldFuncs.phookD3D11DrawIndexedInstancedAlternate);
	}

	bool D3D::CaptureVerificationSource(
		ID3D11Texture2D* sourceTexture,
		const char* anchorName,
		ID3D11Device* captureDevice,
		ID3D11DeviceContext* captureContext)
	{
		auto* device = captureDevice ? captureDevice : g_Device.Get();
		auto* context = captureContext ? captureContext : g_Context.Get();
		if (!sourceTexture || !anchorName || !device || !context) {
			return false;
		}

		const bool isTaaAnchor = std::strcmp(anchorName, "TAA") == 0;
		static std::atomic_bool verifiedFrameworkCapture = false;
		static std::atomic_bool verifiedTaaCapture = false;
		auto& verifiedCapture =
			isTaaAnchor ? verifiedTaaCapture : verifiedFrameworkCapture;
		if (verifiedCapture.load(std::memory_order_acquire)) {
			return true;
		}

		D3D11_TEXTURE2D_DESC sourceDesc{};
		sourceTexture->GetDesc(&sourceDesc);

		// CopyResource requires the complete resource shape to match. Preserve
		// mip, array, format, and sample metadata from the source while
		// removing every bind and CPU-access flag from the private copy.
		D3D11_TEXTURE2D_DESC captureDesc = sourceDesc;
		captureDesc.Usage = D3D11_USAGE_DEFAULT;
		captureDesc.BindFlags = 0;
		captureDesc.CPUAccessFlags = 0;
		captureDesc.MiscFlags = 0;

		// Stage 3 uses a local resource rather than D3D::mCurRTTexture. The TAA
		// callback and Menu Framework callback have been observed on different
		// threads, so sharing the later Stage 4 render resource here would race.
		ComPtr<ID3D11Texture2D> captureTexture;
		const HRESULT createCaptureResult = device->CreateTexture2D(
			&captureDesc,
			nullptr,
			captureTexture.GetAddressOf());
		if (FAILED(createCaptureResult) || !captureTexture.Get()) {
			logger::error(
				"Stage 3 {} capture texture creation failed with HRESULT 0x{:08X}",
				anchorName,
				static_cast<std::uint32_t>(createCaptureResult));
			return false;
		}

		context->CopyResource(captureTexture.Get(), sourceTexture);

		// A D3D11 staging texture cannot be multisampled. The copy itself still
		// proves the source is compatible, but its bytes require a later resolve.
		if (sourceDesc.SampleDesc.Count != 1) {
			logger::warn(
				"Verification stage 3 {} source copy completed: "
				"{}x{}, format={}, samples={}; readback requires a resolve",
				anchorName,
				sourceDesc.Width,
				sourceDesc.Height,
				static_cast<int>(sourceDesc.Format),
				sourceDesc.SampleDesc.Count);
			verifiedCapture.store(true, std::memory_order_release);
			return true;
		}

		// One synchronous readback proves the GPU copy contains source bytes.
		// It runs once per anchor and never binds or draws to a game target.
		D3D11_TEXTURE2D_DESC stagingDesc = captureDesc;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.BindFlags = 0;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ComPtr<ID3D11Texture2D> stagingTexture;
		const HRESULT createStagingResult = device->CreateTexture2D(
			&stagingDesc,
			nullptr,
			stagingTexture.GetAddressOf());
		if (FAILED(createStagingResult) || !stagingTexture.Get()) {
			logger::error(
				"Stage 3 {} staging texture creation failed with HRESULT 0x{:08X}",
				anchorName,
				static_cast<std::uint32_t>(createStagingResult));
			return false;
		}

		context->CopyResource(stagingTexture.Get(), captureTexture.Get());
		D3D11_MAPPED_SUBRESOURCE mapped{};
		const HRESULT mapResult = context->Map(
			stagingTexture.Get(),
			0,
			D3D11_MAP_READ,
			0,
			&mapped);
		if (FAILED(mapResult) || !mapped.pData) {
			logger::error(
				"Stage 3 {} source readback failed with HRESULT 0x{:08X}",
				anchorName,
				static_cast<std::uint32_t>(mapResult));
			return false;
		}

		const auto* centerRow =
			static_cast<const std::uint8_t*>(mapped.pData) +
			(sourceDesc.Height / 2) * mapped.RowPitch;
		const std::size_t bytesToHash =
			std::min<std::size_t>(mapped.RowPitch, 256);
		const auto* centerSample =
			centerRow + (mapped.RowPitch - bytesToHash) / 2;
		std::uint64_t centerHash = 1469598103934665603ULL;
		std::size_t nonZeroBytes = 0;
		for (std::size_t index = 0; index < bytesToHash; ++index) {
			const auto value = centerSample[index];
			centerHash ^= value;
			centerHash *= 1099511628211ULL;
			nonZeroBytes += value != 0;
		}
		context->Unmap(stagingTexture.Get(), 0);

		logger::info(
			"Verification stage 3 {} source capture completed: "
			"{}x{}, format={}, samples={}, centerHash={:016X}, nonzeroBytes={}/{}; "
			"no shaders, RTVs, or draws",
			anchorName,
			sourceDesc.Width,
			sourceDesc.Height,
			static_cast<int>(sourceDesc.Format),
			sourceDesc.SampleDesc.Count,
			centerHash,
			nonZeroBytes,
			bytesToHash);
		verifiedCapture.store(true, std::memory_order_release);
		return true;
	}

	bool D3D::CaptureVerificationTAASource()
	{
		const auto* rendererData = RE::BSGraphics::GetRendererData();
		if (!rendererData || !rendererData->context) {
			return false;
		}

		// This runs inside Fallout 4's TAA Render callback. Query only the
		// render-thread D3D state that the original scope-rendering contract relies on.
		// No camera, weapon, scene graph, or profile object is touched here.
		auto* context =
			static_cast<ID3D11DeviceContext*>(static_cast<void*>(rendererData->context));
		ID3D11RenderTargetView* targets[2]{};
		ID3D11DepthStencilView* depth = nullptr;
		context->OMGetRenderTargets(2, targets, &depth);

		auto releaseTargets = [&] {
			SAFE_RELEASE(targets[0]);
			SAFE_RELEASE(targets[1]);
			SAFE_RELEASE(depth);
		};

		auto* sourceView = targets[1] ? targets[1] : targets[0];
		ID3D11Resource* sourceResource = nullptr;
		if (sourceView) {
			sourceView->GetResource(&sourceResource);
		}

		ComPtr<ID3D11Texture2D> sourceTexture;
		if (sourceResource) {
			sourceResource->QueryInterface(
				__uuidof(ID3D11Texture2D),
				reinterpret_cast<void**>(sourceTexture.GetAddressOf()));
			sourceResource->Release();
		}
		releaseTargets();
		if (!sourceTexture.Get()) {
			return false;
		}

		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> immediateContext;
		sourceTexture->GetDevice(device.GetAddressOf());
		if (device.Get()) {
			device->GetImmediateContext(immediateContext.GetAddressOf());
		}
		return CaptureVerificationSource(
			sourceTexture.Get(),
			"TAA",
			device.Get(),
			immediateContext.Get());
	}

	void D3D::Render()
	{
		lastRenderProducedComposite = false;
		const auto& verification = MagnaScope::GetSettings();
		if (!verification.AllowsRenderer()) {
			return;
		}

		if (!g_Swapchain.Get() || !g_Device.Get() || !g_Context.Get()) {
			const auto* rendererData = RE::BSGraphics::GetRendererData();
			const auto* renderWindow = RE::BSGraphics::GetCurrentRendererWindow();
			if (!rendererData || !renderWindow || !renderWindow->swapChain ||
				!rendererData->device || !rendererData->context) {
				return;
			}
			g_Swapchain = static_cast<IDXGISwapChain*>(static_cast<void*>(renderWindow->swapChain));
			g_Device = static_cast<ID3D11Device*>(static_cast<void*>(rendererData->device));
			g_Context = static_cast<ID3D11DeviceContext*>(static_cast<void*>(rendererData->context));
		}

		if (!sdh) {
			sdh = ScopeData::ScopeDataHandler::GetSingleton();
		}

		if (bQueryRender && sdh) {
			if (!verification.AllowsComposite() && renderedAtTAAThisFrame) {
				if (!isEnableRender || !pcam || !player) {
					return;
				}
				const auto* currData = sdh->GetCurrentScopeProfile();
				if (!currData || !currData->containAlladditionalKeywords) {
					return;
				}

				// The original scope-rendering callback runs after the game's TAA Render
				// method. At that point its output remains bound to the output
				// merger. Read those references without changing any state,
				// choose the same RT1-preferred source contract as upstream,
				// and release every reference before returning.
				ID3D11RenderTargetView* taaTargets[2]{};
				ID3D11DepthStencilView* taaDepth = nullptr;
				g_Context->OMGetRenderTargets(2, taaTargets, &taaDepth);
				auto releaseTaaTargets = [&] {
					SAFE_RELEASE(taaTargets[0]);
					SAFE_RELEASE(taaTargets[1]);
					SAFE_RELEASE(taaDepth);
				};

				auto* sourceView = taaTargets[1] ? taaTargets[1] : taaTargets[0];
				ID3D11Resource* sourceResource = nullptr;
				if (sourceView) {
					sourceView->GetResource(&sourceResource);
				}
				ComPtr<ID3D11Texture2D> taaSource;
				if (sourceResource) {
					sourceResource->QueryInterface(
						__uuidof(ID3D11Texture2D),
						reinterpret_cast<void**>(taaSource.GetAddressOf()));
					sourceResource->Release();
				}

				const bool captured =
					CaptureVerificationSource(taaSource.Get(), "TAA");
				releaseTaaTargets();
				if (!captured) {
					logger::warn(
						"Stage 3b TAA callback found no compatible bound color target; "
						"framework Present capture will be used");
					renderedAtTAAThisFrame = false;
				}
				return;
			}

			auto* activeSwapChain =
				lastPresentedSwapChain.load(std::memory_order_acquire);
			if (!activeSwapChain) {
				activeSwapChain = g_Swapchain.Get();
			}
			if (!activeSwapChain) {
				return;
			}

			if (activeSwapChain != backBufferOwnerSwapChain) {
				// ENB/upscaler proxies can present through a different chain
				// object than BSGraphics exposes. The Present argument is the
				// authoritative displayed chain; invalidate every object tied
				// to the previous chain when that identity changes.
				m_pRenderTargetView.Reset();
				mBackBuffer.Reset();
				mBackBufferIndex = UINT_MAX;
				bHasGetBackBuffer = false;
				backBufferOwnerSwapChain = activeSwapChain;
				getBufferReferenceContractProbed = false;
				getBufferAddsReference = true;
				logger::info(
					"Scope swap-chain owner: renderer={:p}, presented={:p}",
					static_cast<void*>(g_Swapchain.Get()),
					static_cast<void*>(activeSwapChain));
			}

			DXGI_SWAP_CHAIN_DESC swapChainDesc{};
			const bool hasSwapChainDesc =
				SUCCEEDED(activeSwapChain->GetDesc(&swapChainDesc));
			const bool isFlipModel =
				hasSwapChainDesc &&
				(swapChainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
					swapChainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
			constexpr UINT currentBackBufferIndex = 0;

			if (!verification.AllowsComposite()) {
				// Stage 3a has no ResizeBuffers hook. Do not retain a back
				// buffer reference between framework callbacks because doing
				// so can prevent a legitimate ResizeBuffers call from
				// succeeding during an alt-tab or resolution change.
				mBackBuffer.Reset();
				bHasGetBackBuffer = false;
			}

			if (!bHasGetBackBuffer || isFlipModel) {
				// Stay on the IDXGISwapChain contract. AAA Frame Generation's
				// DXGISwapChainProxy implements only that base interface: its
				// QueryInterface forwards success for IDXGISwapChain3 but then
				// substitutes the base-only proxy pointer. Calling
				// GetCurrentBackBufferIndex through that pointer jumps through
				// a nonexistent vtable slot and crashes. The proxy's verified
				// GetBuffer implementation ignores the index and returns its
				// actual D3D11 presentation texture, matching F4SE Menu
				// Framework's proven GetBuffer(0) handling.
				//
				// Real flip-model chains rotate which texture GetBuffer(0)
				// exposes, so refresh the texture and RTV each presented frame.
				// Fallout 4 OG's normal discard-model chain keeps the cached
				// fast path.
				m_pRenderTargetView.Reset();
				mBackBuffer.Reset();
				ID3D11Texture2D* acquiredBackBuffer =
					AcquireBackBuffer(activeSwapChain, currentBackBufferIndex);
				if (!acquiredBackBuffer) {
					logger::error(
						"Swap chain did not return back buffer {}",
						currentBackBufferIndex);
					return;
				}

				// Take our own explicit reference regardless of the proxy's
				// GetBuffer behavior, then balance only the reference actually
				// returned by GetBuffer.
				mBackBuffer = acquiredBackBuffer;
				ReleaseAcquiredBackBuffer(acquiredBackBuffer);
				if (verification.AllowsComposite()) {
					const HRESULT createRtvResult = g_Device->CreateRenderTargetView(
						mBackBuffer.Get(),
						nullptr,
						m_pRenderTargetView.ReleaseAndGetAddressOf());
					if (FAILED(createRtvResult)) {
						logger::error(
							"Scope back-buffer RTV creation failed with HRESULT 0x{:08X}",
							static_cast<std::uint32_t>(createRtvResult));
						return;
					}
				}

				static std::once_flag loggedSwapChainContract;
				std::call_once(loggedSwapChainContract, [&] {
					if (!hasSwapChainDesc) {
						swapChainDesc = {};
					}
					logger::info(
						"Scope final-frame target: swapEffect={}, buffers={}, GetBufferIndex={}{}",
						static_cast<int>(swapChainDesc.SwapEffect),
						swapChainDesc.BufferCount,
						currentBackBufferIndex,
						isFlipModel ? " (refreshed each frame)" : "");
				});

				mBackBufferIndex = currentBackBufferIndex;
				bHasGetBackBuffer = true;
			}

			if (isEnableRender && pcam && player) {
				auto currData = sdh->GetCurrentScopeProfile();

				if (!currData || !currData->containAlladditionalKeywords)
					return;

				if (!verification.AllowsComposite()) {
					// Stage 3a is intentionally smaller than the original scope-rendering
					// render contract. It validates only the displayed
					// back-buffer identity and a GPU copy issued from F4SE Menu
					// Framework's before-render callback. It creates no RTV or
					// SRV, initializes no shaders or constant buffers, and
					// changes no pipeline state.
					CaptureVerificationSource(mBackBuffer.Get(), "FrameworkPresent");
					// Stage 3a deliberately owns no resize hook, so release the
					// displayed back buffer before returning to the framework.
					mBackBuffer.Reset();
					bHasGetBackBuffer = false;
					return;
				}

				if (bIsFirst) {
					OnResize();
					if (!InitEffect() || !InitLegacyEffect() || !InitResource()) {
						logger::error("Scope GPU resource initialization failed; rendering disabled");
						isEnableRender = false;
						return;
					}
				}

				ScopedContextState contextState(g_Context.Get());
				rtTexture2D.Reset();

				SAFE_RELEASE(tempRt[0]);
				SAFE_RELEASE(tempRt[1]);
				SAFE_RELEASE(tempSV);
				g_Context->OMGetRenderTargets(2, tempRt, &tempSV);
				ScopedRenderTargetReferences renderTargetReferences;
				if (!renderedAtTAAThisFrame) {
					// Present anchor: the game's mid-frame render targets have
					// already been consumed by the time Present runs, so
					// compositing into whatever is still bound never reaches
					// the screen. The swap chain back buffer is the only
					// surface that still gets displayed; use it as both the
					// source and (below, in the reticle passes) the target.
					rtTexture2D = mBackBuffer;
				} else {
					ID3D11Resource* rtResource = nullptr;
					if (upscalerMod) {
						if (tempRt[0] != nullptr) {
							tempRt[0]->GetResource(&rtResource);  // 获取纹理接口
						}
					} else {
						auto* sourceView = tempRt[1] ? tempRt[1] : tempRt[0];
						if (sourceView != nullptr) {
							sourceView->GetResource(&rtResource);  // 获取纹理接口
						}
					}
					if (rtResource != nullptr) {
						rtResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)rtTexture2D.GetAddressOf());
						rtResource->Release();  // 释放原始资源引用
					}
				}
				if (!rtTexture2D.Get()) {
					logger::warn("No compatible render target was available for the scope pass");
					return;
				}
				D3D11_TEXTURE2D_DESC originalDesc;
				rtTexture2D->GetDesc(&originalDesc);  // 获取原纹理参数
				rtTextureDesc = originalDesc;
				rtTextureDesc.MipLevels = 1;
				rtTextureDesc.ArraySize = 1;
				rtTextureDesc.Usage = D3D11_USAGE_DEFAULT;
				rtTextureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
				rtTextureDesc.CPUAccessFlags = 0;
				rtTextureDesc.MiscFlags = 0;

				bool recreateCapture =
					!mCurRTTexture.Get() || !mShaderResourceView.Get();
				if (!recreateCapture) {
					D3D11_TEXTURE2D_DESC captureDesc{};
					mCurRTTexture->GetDesc(&captureDesc);
					recreateCapture =
						captureDesc.Width != rtTextureDesc.Width ||
						captureDesc.Height != rtTextureDesc.Height ||
						captureDesc.Format != rtTextureDesc.Format ||
						captureDesc.SampleDesc.Count != rtTextureDesc.SampleDesc.Count ||
						captureDesc.SampleDesc.Quality != rtTextureDesc.SampleDesc.Quality;
				}
				if (recreateCapture) {
					mShaderResourceView.Reset();
					mCurRTTexture.Reset();
					HR(g_Device->CreateTexture2D(
						&rtTextureDesc,
						nullptr,
						mCurRTTexture.GetAddressOf()));
					HR(g_Device->CreateShaderResourceView(
						mCurRTTexture.Get(),
						nullptr,
						mShaderResourceView.GetAddressOf()));
				}

				bIsFirst = false;

				g_Context->CopyResource(mCurRTTexture.Get(), rtTexture2D.Get());
				UpdateScene(currData);

				// One-shot diagnostics on the 30th scoped frame (a steady-state
				// frame, after any aim transition). Reports the compositing
				// configuration, probes the captured source pixels before the
				// draw, and probes the composite target after the draw. If the
				// two probes match, the pass changed no pixels and the problem
				// is in the draw itself, not in what reaches the screen later.
				static const ScopeData::ScopeProfile* diagnosedProfile = nullptr;
				static int diagCountdown = 30;
				if (diagnosedProfile != currData) {
					diagnosedProfile = currData;
					diagCountdown = 30;
				}
				const bool diagFrame = diagCountdown > 0 && --diagCountdown == 0;
				struct ProbeResult
				{
					bool valid = false;
					std::array<std::uint64_t, 3> hashes{};
				};
				const auto probeLens = [&](ID3D11Texture2D* texture, const char* label) {
					ProbeResult result{};
					D3D11_TEXTURE2D_DESC stagingDesc = rtTextureDesc;
					stagingDesc.Usage = D3D11_USAGE_STAGING;
					stagingDesc.BindFlags = 0;
					stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					stagingDesc.MiscFlags = 0;
					ComPtr<ID3D11Texture2D> stagingTexture;
					if (SUCCEEDED(g_Device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.GetAddressOf()))) {
						g_Context->CopyResource(stagingTexture.Get(), texture);
						D3D11_MAPPED_SUBRESOURCE mapped{};
						if (SUCCEEDED(g_Context->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
							// Probe three blocks inside the actual lens. The
							// optical center maps to itself under magnification,
							// so a center-only probe cannot prove a zoom pass.
							// The two off-center blocks must change at zoom > 1.
							const auto* base = static_cast<const std::uint8_t*>(mapped.pData);
							const bool validX =
								scopeData.ScopeScreenPos.x >= 0.0F &&
								scopeData.ScopeScreenPos.x < stagingDesc.Width;
							const bool validY =
								scopeData.ScopeScreenPos.y >= 0.0F &&
								scopeData.ScopeScreenPos.y < stagingDesc.Height;
							const int lensX = validX ?
							                      static_cast<int>(scopeData.ScopeScreenPos.x) :
							                      static_cast<int>(stagingDesc.Width / 2);
							const int lensY = validY ?
							                      static_cast<int>(scopeData.ScopeScreenPos.y) :
							                      static_cast<int>(stagingDesc.Height / 2);
							const int radius = std::max(
								16,
								static_cast<int>(
									scopeData.ScopeEffect_Size.x *
									(stagingDesc.Height / 1080.0F) * 0.5F));
							const std::array<std::pair<int, int>, 3> points{ { { lensX, lensY },
								{ lensX + radius / 2, lensY },
								{ lensX, lensY - radius / 2 } } };
							std::array<std::array<std::uint64_t, 3>, 3> sums{};
							result.hashes.fill(1469598103934665603ULL);
							for (std::size_t point = 0; point < points.size(); ++point) {
								const int startX = std::clamp(
									points[point].first - 8,
									0,
									static_cast<int>(stagingDesc.Width) - 16);
								const int startY = std::clamp(
									points[point].second - 8,
									0,
									static_cast<int>(stagingDesc.Height) - 16);
								for (int y = 0; y < 16; ++y) {
									const auto* row =
										base + (startY + y) * mapped.RowPitch + startX * 4;
									for (int x = 0; x < 16; ++x) {
										for (int channel = 0; channel < 4; ++channel) {
											const auto value = row[x * 4 + channel];
											result.hashes[point] ^= value;
											result.hashes[point] *= 1099511628211ULL;
											if (channel < 3) {
												sums[point][channel] += value;
											}
										}
									}
								}
							}
							g_Context->Unmap(stagingTexture.Get(), 0);
							result.valid = true;
							logger::info(
								"Scope {} probe at ({}, {}): center={} {} {}, right={} {} {}, upper={} {} {}, hashes={:016X}/{:016X}/{:016X}",
								label,
								lensX,
								lensY,
								sums[0][0] / 256,
								sums[0][1] / 256,
								sums[0][2] / 256,
								sums[1][0] / 256,
								sums[1][1] / 256,
								sums[1][2] / 256,
								sums[2][0] / 256,
								sums[2][1] / 256,
								sums[2][2] / 256,
								result.hashes[0],
								result.hashes[1],
								result.hashes[2]);
						} else {
							logger::warn("Scope {} probe could not map the staging texture", label);
						}
					}
					return result;
				};
				ProbeResult sourceProbe{};
				if (diagFrame) {
					logger::info(
						"Scope pass: {}x{} format {} ({} anchor, legacy={}, zoom={:.2f}, merge={}, circle {:.0f}x{:.0f} at ({:.3f}, {:.3f}))",
						originalDesc.Width,
						originalDesc.Height,
						static_cast<int>(originalDesc.Format),
						renderedAtTAAThisFrame ? "TAA" : "Present",
						bLegacyMode,
						scopeData.ScopeEffect_Zoom,
						scopeData.EnableMerge,
						scopeData.ScopeEffect_Size.x,
						scopeData.ScopeEffect_Size.y,
						scopeData.ScopeScreenPos.x,
						scopeData.ScopeScreenPos.y);
					sourceProbe = probeLens(mCurRTTexture.Get(), "source");
				}

				if (bLegacyMode) {
					lastRenderProducedComposite =
						D3DInstance->RenderToReticleTexture();
				} else {
					D3DInstance->ScreenTextureMod();
					D3DInstance->RenderToReticleTextureNew(targetIndexCount, targetStartIndexLocation, targetBaseVertexLocation);
					lastRenderProducedComposite =
						targetVS.Get() &&
						targetVertexConstBufferOutPut &&
						targetIndexCount > 0;
				}
				// A failed automatic STS call remains eligible for a later verified
				// frame anchor.
				renderPassHandledThisFrame =
					bLegacyMode ? lastRenderProducedComposite : true;

				if (diagFrame) {
					// rtTexture2D is the composite destination at both anchors
					// (the back buffer at Present, the bound target at TAA).
					const auto targetProbe = probeLens(rtTexture2D.Get(), "target-after-draw");
					if (sourceProbe.valid && targetProbe.valid) {
						logger::info(
							"Scope draw changed sampled blocks: center={}, right={}, upper={}",
							sourceProbe.hashes[0] != targetProbe.hashes[0],
							sourceProbe.hashes[1] != targetProbe.hashes[1],
							sourceProbe.hashes[2] != targetProbe.hashes[2]);
					}
				}

				g_Context->OMSetRenderTargets(2, tempRt, tempSV);
			}
		}
	}

	void D3D::RenderFromFramework()
	{
		if (!frameworkRenderAnchor) {
			return;
		}

		static std::once_flag loggedFrameworkAnchor;
		std::call_once(loggedFrameworkAnchor, [] {
			logger::info("F4SE Menu Framework before-render anchor is dispatching");
		});

		const auto& verification = MagnaScope::GetSettings();
		if (verification.AllowsRenderer() && !verification.AllowsComposite()) {
			// Stage 3 owns no Present hook, so this verified framework event is
			// its frame boundary. A successful TAA capture arrives earlier in
			// the same frame and wins. Otherwise the already passed framework
			// Present source remains the fallback.
			lastRenderProducedComposite = false;
			const bool taaCaptured =
				taaVerificationCapturedSinceFramework.exchange(
					false,
					std::memory_order_acq_rel);
			if (isEnableRender.load(std::memory_order_acquire) && !taaCaptured) {
				Render();
			}
			renderedAtTAAThisFrame = false;
			compositedThisFrame = false;
			return;
		}

		// Stage 4 may prefer an earlier TAA result and keeps the shared guard
		// until MagnaScope's own Present callback ends the frame.
		if (isEnableRender && !renderPassHandledThisFrame) {
			Render();
			compositedThisFrame = lastRenderProducedComposite;
		}
	}

	void D3D::PublishLensProjection(
		float centerX,
		float centerY,
		float aimCenterX,
		float aimCenterY,
		float radiusX,
		float radiusY,
		bool automaticSTS,
		float activationProgress,
		const PhysicalEyeBoxSample& physicalEyeBox)
	{
		// Begin an odd/even seqlock publication. A readiness flag alone is not
		// a coherent snapshot: the render thread can read the previous true
		// immediately before this writer clears it, then mix old lens data with
		// new recoil telemetry. The sequence lets the reader reject that frame.
		projectedLensSequence.fetch_add(1U, std::memory_order_acq_rel);
		projectedTrackingReady.store(false, std::memory_order_release);
		projectedLensX.store(centerX, std::memory_order_relaxed);
		projectedLensY.store(centerY, std::memory_order_relaxed);
		projectedAimX.store(aimCenterX, std::memory_order_relaxed);
		projectedAimY.store(aimCenterY, std::memory_order_relaxed);
		projectedLensRadiusX.store(radiusX, std::memory_order_relaxed);
		projectedLensRadiusY.store(radiusY, std::memory_order_relaxed);
		projectedActivationProgress.store(
			std::clamp(activationProgress, 0.0F, 1.0F),
			std::memory_order_relaxed);
		projectedSourceWidth.store(
			static_cast<float>(windowWidth),
			std::memory_order_relaxed);
		projectedSourceHeight.store(
			static_cast<float>(windowHeight),
			std::memory_order_relaxed);
		projectedAutomaticSTS.store(automaticSTS, std::memory_order_relaxed);
		projectedEyeOffsetX.store(
			physicalEyeBox.eyeOffsetX,
			std::memory_order_relaxed);
		projectedEyeOffsetY.store(
			physicalEyeBox.eyeOffsetY,
			std::memory_order_relaxed);
		projectedEyeReliefDelta.store(
			physicalEyeBox.eyeReliefDelta,
			std::memory_order_relaxed);
		projectedLensBasisXX.store(
			physicalEyeBox.lensBasisXX,
			std::memory_order_relaxed);
		projectedLensBasisXY.store(
			physicalEyeBox.lensBasisXY,
			std::memory_order_relaxed);
		projectedLensBasisZX.store(
			physicalEyeBox.lensBasisZX,
			std::memory_order_relaxed);
		projectedLensBasisZY.store(
			physicalEyeBox.lensBasisZY,
			std::memory_order_relaxed);
		projectedPhysicalEyeBoxBlend.store(
			std::clamp(physicalEyeBox.blend, 0.0F, 1.0F),
			std::memory_order_relaxed);
		projectedPhysicalEyeBoxReady.store(
			physicalEyeBox.valid,
			std::memory_order_relaxed);
		projectedTrackingReady.store(
			activationProgress > 0.0F,
			std::memory_order_release);
		projectedLensSequence.fetch_add(1U, std::memory_order_release);
	}

	void D3D::InvalidateLensProjection()
	{
		// Invalidating readiness is sufficient to make every visual consumer
		// fail closed. Coordinates remain available for diagnostics but cannot
		// accidentally draw while the player is at the hip or changing optics.
		projectedLensSequence.fetch_add(1U, std::memory_order_acq_rel);
		projectedTrackingReady.store(false, std::memory_order_release);
		projectedAutomaticSTS.store(false, std::memory_order_relaxed);
		projectedPhysicalEyeBoxReady.store(false, std::memory_order_relaxed);
		projectedPhysicalEyeBoxBlend.store(0.0F, std::memory_order_relaxed);
		projectedActivationProgress.store(0.0F, std::memory_order_relaxed);
		projectedLensSequence.fetch_add(1U, std::memory_order_release);
	}

	D3D::LensProjectionSnapshot D3D::GetLensProjectionSnapshot() const
	{
		// The game thread publishes this snapshot through a small seqlock while
		// the render thread consumes it. A retry collision is not a semantic
		// invalidation. Reusing the last coherent snapshot for that draw keeps a
		// sharp camera move from producing a one-frame center snap.
		static thread_local LensProjectionSnapshot lastCoherentSnapshot{};
		for (std::uint32_t attempt = 0U; attempt < 3U; ++attempt) {
			const auto sequenceBefore =
				projectedLensSequence.load(std::memory_order_acquire);
			if ((sequenceBefore & 1U) != 0U) {
				continue;
			}

			LensProjectionSnapshot result{};
			result.trackingReady =
				projectedTrackingReady.load(std::memory_order_relaxed);
			result.centerX = projectedLensX.load(std::memory_order_relaxed);
			result.centerY = projectedLensY.load(std::memory_order_relaxed);
			result.aimCenterX =
				projectedAimX.load(std::memory_order_relaxed);
			result.aimCenterY =
				projectedAimY.load(std::memory_order_relaxed);
			result.radiusX =
				projectedLensRadiusX.load(std::memory_order_relaxed);
			result.radiusY =
				projectedLensRadiusY.load(std::memory_order_relaxed);
			result.activationProgress =
				projectedActivationProgress.load(
					std::memory_order_relaxed);
			result.sourceWidth =
				projectedSourceWidth.load(std::memory_order_relaxed);
			result.sourceHeight =
				projectedSourceHeight.load(std::memory_order_relaxed);
			result.renderEnabled =
				isEnableRender.load(std::memory_order_relaxed);
			result.automaticSTS =
				projectedAutomaticSTS.load(std::memory_order_relaxed);
			result.eyeOffsetX =
				projectedEyeOffsetX.load(std::memory_order_relaxed);
			result.eyeOffsetY =
				projectedEyeOffsetY.load(std::memory_order_relaxed);
			result.eyeReliefDelta =
				projectedEyeReliefDelta.load(std::memory_order_relaxed);
			result.lensBasisXX =
				projectedLensBasisXX.load(std::memory_order_relaxed);
			result.lensBasisXY =
				projectedLensBasisXY.load(std::memory_order_relaxed);
			result.lensBasisZX =
				projectedLensBasisZX.load(std::memory_order_relaxed);
			result.lensBasisZY =
				projectedLensBasisZY.load(std::memory_order_relaxed);
			result.physicalEyeBoxBlend =
				projectedPhysicalEyeBoxBlend.load(
					std::memory_order_relaxed);
			result.physicalEyeBoxReady =
				projectedPhysicalEyeBoxReady.load(
					std::memory_order_relaxed);

			const auto sequenceAfter =
				projectedLensSequence.load(std::memory_order_acquire);
			if (sequenceBefore == sequenceAfter &&
				(sequenceAfter & 1U) == 0U) {
				lastCoherentSnapshot = result;
				return result;
			}
		}

		return lastCoherentSnapshot;
	}

	void D3D::PublishAutomaticSTSGeometry(
		RE::NiAVObject* renderSurface,
		const std::vector<RE::NiAVObject*>& reticleSurfaces,
		RE::NiAVObject* aimingHousingSurface)
	{
		// BSGeometry::rendererData is documented by the vendored CommonLibF4
		// layout as a BSGraphics::TriShape allocation. Read it only on the game
		// thread while the selected scene objects are alive. The render thread
		// receives opaque D3D identities and never follows a scene pointer.
		const auto publishIdentity = [](
										 RE::NiAVObject* object,
										 std::atomic<std::uintptr_t>& publishedVertexBuffer,
										 std::atomic<std::uintptr_t>& publishedIndexBuffer,
										 std::atomic_uint32_t& publishedIndexCount,
										 std::atomic_uint32_t& publishedVertexStride,
										 std::atomic_uint32_t& publishedVertexDataOffset,
										 std::atomic_uint32_t& publishedIndexDataOffset,
										 std::atomic_bool& publishedReady,
										 const char* label,
									 std::atomic_uint32_t* publishedVertexCount = nullptr,
									 std::atomic_uint64_t* publishedVertexDescriptor = nullptr,
									 std::atomic_uint64_t* publishedGeneration = nullptr) {
			auto* triShape = object ? object->IsTriShape() : nullptr;
			auto* rendererShape =
				triShape && triShape->rendererData ?
					static_cast<RE::BSGraphics::TriShape*>(
						triShape->rendererData) :
					nullptr;
			auto* vertexBuffer =
				rendererShape && rendererShape->vertexBuffer ?
					reinterpret_cast<ID3D11Buffer*>(
						rendererShape->vertexBuffer->buffer) :
					nullptr;
			auto* indexBuffer =
				rendererShape && rendererShape->indexBuffer ?
					reinterpret_cast<ID3D11Buffer*>(
						rendererShape->indexBuffer->buffer) :
					nullptr;
			const std::uint32_t indexCount =
				triShape ? triShape->numTriangles * 3U : 0U;
			const std::uint32_t vertexCount =
				triShape ? triShape->numVertices : 0U;
			const std::uint32_t vertexStride =
				triShape ? triShape->vertexDesc.GetSize() : 0U;
			const std::uint64_t vertexDescriptor =
				triShape ? triShape->vertexDesc.desc : 0U;
			const std::uint32_t vertexDataOffset =
				rendererShape && rendererShape->vertexBuffer ?
					rendererShape->vertexBuffer->dataOffset :
					0U;
			const std::uint32_t indexDataOffset =
				rendererShape && rendererShape->indexBuffer ?
					rendererShape->indexBuffer->dataOffset :
					0U;

			if (!vertexBuffer || !indexBuffer || indexCount == 0U ||
				vertexCount == 0U ||
				vertexStride == 0U) {
				const bool wasReady =
					publishedReady.exchange(
						false,
						std::memory_order_acq_rel);
				if (wasReady && publishedGeneration) {
					publishedGeneration->fetch_add(
						1U,
						std::memory_order_acq_rel);
				}
				return false;
			}

			const auto vertexAddress =
				reinterpret_cast<std::uintptr_t>(vertexBuffer);
			const auto previousVertex =
				publishedVertexBuffer.load(std::memory_order_relaxed);
			const bool metadataChanged =
				previousVertex != vertexAddress ||
				publishedIndexBuffer.load(std::memory_order_relaxed) !=
					reinterpret_cast<std::uintptr_t>(indexBuffer) ||
				publishedIndexCount.load(std::memory_order_relaxed) !=
					indexCount ||
				publishedVertexStride.load(std::memory_order_relaxed) !=
					vertexStride ||
				publishedVertexDataOffset.load(
					std::memory_order_relaxed) != vertexDataOffset ||
				publishedIndexDataOffset.load(
					std::memory_order_relaxed) != indexDataOffset ||
				(publishedVertexCount &&
					publishedVertexCount->load(
						std::memory_order_relaxed) != vertexCount) ||
				(publishedVertexDescriptor &&
					publishedVertexDescriptor->load(
						std::memory_order_relaxed) !=
						vertexDescriptor);
			if (!metadataChanged &&
				publishedReady.load(std::memory_order_acquire)) {
				return true;
			}

			// Publish readiness last. DrawIndexedHook acquires it before
			// comparing the relaxed fields, so a new object cannot be paired
			// with the previous object's count or suballocation.
			publishedReady.store(false, std::memory_order_release);
			if (publishedGeneration) {
				// Increment before publishing any changed fields. Readers
				// validate the generation both before and after their relaxed
				// snapshot, so they cannot accept mixed old/new metadata.
				publishedGeneration->fetch_add(
					1U,
					std::memory_order_acq_rel);
			}
			publishedVertexBuffer.store(
				vertexAddress,
				std::memory_order_relaxed);
			publishedIndexBuffer.store(
				reinterpret_cast<std::uintptr_t>(indexBuffer),
				std::memory_order_relaxed);
			publishedIndexCount.store(indexCount, std::memory_order_relaxed);
			publishedVertexStride.store(
				vertexStride,
				std::memory_order_relaxed);
			publishedVertexDataOffset.store(
				vertexDataOffset,
				std::memory_order_relaxed);
			publishedIndexDataOffset.store(
				indexDataOffset,
				std::memory_order_relaxed);
			if (publishedVertexCount) {
				publishedVertexCount->store(
					vertexCount,
					std::memory_order_relaxed);
			}
			if (publishedVertexDescriptor) {
				publishedVertexDescriptor->store(
					vertexDescriptor,
					std::memory_order_relaxed);
			}
			publishedReady.store(true, std::memory_order_release);

			if (previousVertex != vertexAddress) {
				logger::info(
					"Published automatic STS {} identity: surface={}, "
					"VB={:p}, IB={:p}, vertices={}, indices={}, stride={}, "
					"vertexDataOffset={}, indexDataOffset={}",
					label,
					object->name.c_str(),
					static_cast<void*>(vertexBuffer),
					static_cast<void*>(indexBuffer),
					vertexCount,
					indexCount,
					vertexStride,
					vertexDataOffset,
					indexDataOffset);
			}
			return true;
		};

		if (!publishIdentity(
				renderSurface,
				automaticSTSVertexBuffer,
				automaticSTSIndexBuffer,
				automaticSTSIndexCount,
				automaticSTSVertexStride,
				automaticSTSVertexDataOffset,
				automaticSTSIndexDataOffset,
				automaticSTSGeometryReady,
				"ScopeFade")) {
			InvalidateAutomaticSTSGeometry();
			return;
		}

		// Reticle:0 is only one possible leaf. STS authors commonly split the
		// etched marks, illuminated dot, glow, and recoil variants across a
		// ReticleNode subtree. Publishing one leaf left the other authored draws
		// in the late scene copy, where scene magnification necessarily enlarged
		// them. Publish the complete renderable set before marking it readable.
		struct ReticleIdentityValue
		{
			std::uintptr_t vertexBuffer{ 0U };
			std::uintptr_t indexBuffer{ 0U };
			std::uint32_t indexCount{ 0U };
			std::uint32_t vertexStride{ 0U };
			std::uint32_t vertexDataOffset{ 0U };
			std::uint32_t indexDataOffset{ 0U };
		};
		std::vector<ReticleIdentityValue> reticleIdentities;
		reticleIdentities.reserve(std::min(
			reticleSurfaces.size(),
			kMaxAutomaticSTSReticleGeometries));
		for (auto* reticleSurface : reticleSurfaces) {
			if (reticleIdentities.size() >=
				kMaxAutomaticSTSReticleGeometries) {
				break;
			}
			auto* triShape = reticleSurface ?
				reticleSurface->IsTriShape() : nullptr;
			auto* rendererShape =
				triShape && triShape->rendererData ?
					static_cast<RE::BSGraphics::TriShape*>(
						triShape->rendererData) :
					nullptr;
			auto* vertexBuffer =
				rendererShape && rendererShape->vertexBuffer ?
					reinterpret_cast<ID3D11Buffer*>(
						rendererShape->vertexBuffer->buffer) :
					nullptr;
			auto* indexBuffer =
				rendererShape && rendererShape->indexBuffer ?
					reinterpret_cast<ID3D11Buffer*>(
						rendererShape->indexBuffer->buffer) :
					nullptr;
			const auto indexCount =
				triShape ? triShape->numTriangles * 3U : 0U;
			const auto vertexStride =
				triShape ? triShape->vertexDesc.GetSize() : 0U;
			if (!vertexBuffer || !indexBuffer || indexCount == 0U ||
				vertexStride == 0U) {
				continue;
			}
			reticleIdentities.push_back({
				reinterpret_cast<std::uintptr_t>(vertexBuffer),
				reinterpret_cast<std::uintptr_t>(indexBuffer),
				indexCount,
				vertexStride,
				rendererShape->vertexBuffer->dataOffset,
				rendererShape->indexBuffer->dataOffset
			});
		}
		if (reticleSurfaces.size() > kMaxAutomaticSTSReticleGeometries) {
			static std::once_flag loggedReticleGeometryOverflow;
			std::call_once(loggedReticleGeometryOverflow, [] {
				logger::warn(
					"An STS reticle subtree exceeded {} renderable shapes; "
					"only the bounded prefix can be isolated",
					kMaxAutomaticSTSReticleGeometries);
			});
		}

		// The odd sequence invalidates readers while every atomic entry is
		// replaced. Count is published last and the following even sequence
		// commits the set as one equipment-generation snapshot.
		automaticSTSReticleSetSequence.fetch_add(
			1U,
			std::memory_order_acq_rel);
		automaticSTSReticleGeometryCount.store(
			0U,
			std::memory_order_release);
		for (std::size_t index = 0U; index < reticleIdentities.size(); ++index) {
			const auto& source = reticleIdentities[index];
			auto& destination = automaticSTSReticleGeometries[index];
			destination.vertexBuffer.store(
				source.vertexBuffer,
				std::memory_order_relaxed);
			destination.indexBuffer.store(
				source.indexBuffer,
				std::memory_order_relaxed);
			destination.indexCount.store(
				source.indexCount,
				std::memory_order_relaxed);
			destination.vertexStride.store(
				source.vertexStride,
				std::memory_order_relaxed);
			destination.vertexDataOffset.store(
				source.vertexDataOffset,
				std::memory_order_relaxed);
			destination.indexDataOffset.store(
				source.indexDataOffset,
				std::memory_order_relaxed);
		}
		automaticSTSReticleGeometryCount.store(
			static_cast<std::uint32_t>(reticleIdentities.size()),
			std::memory_order_release);
		automaticSTSReticleSetSequence.fetch_add(
			1U,
			std::memory_order_release);

		// Keep the first-leaf metadata for the retired CPU scaling cache and
		// diagnostics. Runtime matching uses the complete set above.
		publishIdentity(
			reticleSurfaces.empty() ? nullptr : reticleSurfaces.front(),
			automaticSTSReticleVertexBuffer,
			automaticSTSReticleIndexBuffer,
			automaticSTSReticleIndexCount,
			automaticSTSReticleVertexStride,
			automaticSTSReticleVertexDataOffset,
			automaticSTSReticleIndexDataOffset,
			automaticSTSReticleGeometryReady,
			"Reticle",
			&automaticSTSReticleVertexCount,
			&automaticSTSReticleVertexDescriptor,
			&automaticSTSReticleGeometryGeneration);
		if (!reticleIdentities.empty()) {
			static std::size_t lastLoggedReticleCount = 0U;
			if (lastLoggedReticleCount != reticleIdentities.size()) {
				lastLoggedReticleCount = reticleIdentities.size();
				logger::info(
					"Published automatic STS reticle subtree: {} renderable "
					"draw identities",
					reticleIdentities.size());
			}
		}

		// The direct ScopeAiming child ending in _STS is the authored aiming
		// housing selected by FindSTSAperture. Unlike the front ScopeFade plane,
		// the housing remains visibly present throughout recoil. Stage 4d.2d
		// publishes it only as a candidate injection anchor; this diagnostic
		// build never changes its shaders or submits extra geometry.
		publishIdentity(
			aimingHousingSurface,
			automaticSTSHousingVertexBuffer,
			automaticSTSHousingIndexBuffer,
			automaticSTSHousingIndexCount,
			automaticSTSHousingVertexStride,
			automaticSTSHousingVertexDataOffset,
			automaticSTSHousingIndexDataOffset,
			automaticSTSHousingGeometryReady,
			"AimingHousing");
	}

	void D3D::InvalidateAutomaticSTSGeometry()
	{
		// The opaque addresses are left intact for post-mortem diagnostics.
		// Clearing readiness first prevents a render-thread draw from matching
		// an object that has just been unequipped.
		//
		// This runs on the game thread and takes effect immediately, including
		// partway through a frame the render thread is still submitting. If it
		// lands before ScopeFade -- which is drawn near the very end of the
		// frame -- the aperture is skipped for that frame. Report the
		// transition so a session where the lens never engages shows whether
		// this is firing repeatedly.
		if (automaticSTSGeometryReady.exchange(
				false,
				std::memory_order_acq_rel)) {
			static std::atomic_uint32_t loggedInvalidations{ 0U };
			const auto invalidationIndex =
				loggedInvalidations.fetch_add(1U, std::memory_order_relaxed);
			if (invalidationIndex < 16U) {
				logger::info(
					"Automatic STS geometry readiness cleared "
					"(invalidation {}, draws observed this frame: {})",
					invalidationIndex + 1U,
					automaticSTSObservedDrawsThisFrame.load(
						std::memory_order_relaxed));
			}
		}
		automaticSTSReticleSetSequence.fetch_add(
			1U,
			std::memory_order_acq_rel);
		automaticSTSReticleGeometryCount.store(
			0U,
			std::memory_order_release);
		automaticSTSReticleSetSequence.fetch_add(
			1U,
			std::memory_order_release);
		if (automaticSTSReticleGeometryReady.exchange(
				false,
				std::memory_order_acq_rel)) {
			automaticSTSReticleGeometryGeneration.fetch_add(
				1U,
				std::memory_order_acq_rel);
		}
		automaticSTSHousingGeometryReady.store(
			false,
			std::memory_order_release);
		automaticSTSScopeFadeDrawsThisFrame.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSScopeFadeVisibleLastFrame.store(
			false,
			std::memory_order_release);
		automaticSTSReticleDrawsThisFrame.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSHousingDrawsThisFrame.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSScopeFadeInstancedDrawsThisFrame.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSReticleInstancedDrawsThisFrame.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSHousingInstancedDrawsThisFrame.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSExactScopeFadeReplacementThisFrame.store(
			false,
			std::memory_order_release);
		ClearAutomaticSTSScopeFadeReplay();
		ClearAutomaticSTSReticleReplay();
		ClearAutomaticSTSReticleLayer();
		automaticSTSDrawOrdinalThisFrame.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSLastScopeFadeOrdinal.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSLastReticleOrdinal.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSLastHousingOrdinal.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSObservedDrawsThisFrame.store(
			0U,
			std::memory_order_relaxed);
			automaticSTSObservedInstancedDrawsThisFrame.store(
			0U,
			std::memory_order_relaxed);
		automaticSTSScopeFadeShapedDrawsThisFrame.store(
			0U,
			std::memory_order_relaxed);
	}

	void D3D::PublishAutomaticSTSGunState(
		std::uint32_t gunState) noexcept
	{
		automaticSTSGunState.store(gunState, std::memory_order_release);
	}

	void D3D::SetFrameworkRenderAnchor(bool enabled)
	{
		frameworkRenderAnchor = enabled;
	}

	HRESULT __fastcall D3D::PresentHook(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
	{
		if (!oldFuncs.phookD3D11Present) {
			return DXGI_ERROR_INVALID_CALL;
		}
		bSelfDraw = false;
		lastPresentedSwapChain.store(pSwapChain, std::memory_order_release);

		// Re-read the vtable pointer from the context rather than trusting the
		// one captured at install: this covers a swapped vtable as well as a
		// swapped entry. Once both implementations are hooked this settles to
		// one load and a couple of compares per frame and never fires again.
		if (g_Context.Get()) {
			auto* const contextVTable =
				*reinterpret_cast<DWORD_PTR**>(g_Context.Get());
			if (contextVTable) {
				g_deviceContextVTable = contextVTable;
				BindDrawHookTarget(
					contextVTable,
					12U,
					reinterpret_cast<void*>(DrawIndexedHook),
					reinterpret_cast<void**>(&oldFuncs.phookD3D11DrawIndexed),
					reinterpret_cast<void*>(DrawIndexedHookAlternate),
					reinterpret_cast<void**>(
						&oldFuncs.phookD3D11DrawIndexedAlternate),
					g_drawIndexedBinding,
					"DrawIndexedHook");
				BindDrawHookTarget(
					contextVTable,
					20U,
					reinterpret_cast<void*>(DrawIndexedInstancedHook),
					reinterpret_cast<void**>(
						&oldFuncs.phookD3D11DrawIndexedInstanced),
					reinterpret_cast<void*>(DrawIndexedInstancedHookAlternate),
					reinterpret_cast<void**>(
						&oldFuncs.phookD3D11DrawIndexedInstancedAlternate),
					g_drawIndexedInstancedBinding,
					"DrawIndexedInstancedHook");
			}
		}

		const auto& verification = MagnaScope::GetSettings();
		if (verification.AllowsScopeFadeGeometry() &&
			automaticSTSGeometryReady.load(std::memory_order_acquire)) {
			const auto scopeFadeDraws =
				automaticSTSScopeFadeDrawsThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			automaticSTSScopeFadeVisibleLastFrame.store(
				scopeFadeDraws > 0U,
				std::memory_order_release);
			// Reported every frame rather than through a bounded log budget,
			// because the question it answers -- did See Through Scopes submit
			// the aperture at all -- has to be answerable from a session where
			// the lens never engaged, long after any log budget is spent.
			const auto scopeFadeShapedDraws =
				automaticSTSScopeFadeShapedDrawsThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto observedDraws =
				automaticSTSObservedDrawsThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto observedInstancedDraws =
				automaticSTSObservedInstancedDrawsThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);

			// A frame with the weapon drawn issues thousands of indexed draws.
			// Seeing a couple of dozen means neither of our detours is in the
			// chain -- not that the gate closed, which the gated/observed pair
			// already rules out. Report both hooked implementations against
			// what the vtable holds now, with owning modules, so a third
			// implementation or a failed bind is visible rather than inferred.
			static std::atomic_uint32_t loggedBypassFrames{ 0U };
			if (observedDraws + observedInstancedDraws < 64U &&
				g_deviceContextVTable &&
				loggedBypassFrames.fetch_add(1U, std::memory_order_relaxed) <
					4U) {
				const auto currentDrawIndexed =
					reinterpret_cast<void*>(g_deviceContextVTable[12]);
				const auto currentDrawIndexedInstanced =
					reinterpret_cast<void*>(g_deviceContextVTable[20]);
				logger::warn(
					"Draw hook appears bypassed: observed DI:{} DII:{} this "
					"frame. DrawIndexed hooked {} and {}, vtable now {}; "
					"DrawIndexedInstanced hooked {} and {}, vtable now {}",
					observedDraws,
					observedInstancedDraws,
					DescribeCodeAddress(g_drawIndexedBinding.primaryTarget),
					DescribeCodeAddress(g_drawIndexedBinding.alternateTarget),
					DescribeCodeAddress(currentDrawIndexed),
					DescribeCodeAddress(
						g_drawIndexedInstancedBinding.primaryTarget),
					DescribeCodeAddress(
						g_drawIndexedInstancedBinding.alternateTarget),
					DescribeCodeAddress(currentDrawIndexedInstanced));
			}
			const auto reticleDraws =
				automaticSTSReticleDrawsThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto housingDraws =
				automaticSTSHousingDrawsThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto scopeFadeInstancedDraws =
				automaticSTSScopeFadeInstancedDrawsThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto reticleInstancedDraws =
				automaticSTSReticleInstancedDrawsThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto housingInstancedDraws =
				automaticSTSHousingInstancedDrawsThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto totalAutomaticDraws =
				automaticSTSDrawOrdinalThisFrame.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto scopeFadeOrdinal =
				automaticSTSLastScopeFadeOrdinal.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto reticleOrdinal =
				automaticSTSLastReticleOrdinal.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto housingOrdinal =
				automaticSTSLastHousingOrdinal.exchange(
					0U,
					std::memory_order_acq_rel);
			const auto gunState =
				automaticSTSGunState.load(std::memory_order_acquire);

			static std::atomic_uint32_t previousScopeFadeDraws{
				std::numeric_limits<std::uint32_t>::max()
			};
			static std::atomic_uint32_t previousReticleDraws{
				std::numeric_limits<std::uint32_t>::max()
			};
			static std::atomic_uint32_t previousHousingDraws{
				std::numeric_limits<std::uint32_t>::max()
			};
			static std::atomic_uint32_t previousInstancedDraws{
				std::numeric_limits<std::uint32_t>::max()
			};
			static std::atomic_uint32_t previousGunState{
				std::numeric_limits<std::uint32_t>::max()
			};
			static std::atomic_uint32_t previousShapedDraws{
				std::numeric_limits<std::uint32_t>::max()
			};
			const bool firingSighted =
				gunState ==
				static_cast<std::uint32_t>(RE::GUN_STATE::kFireSighted);
			const bool changed =
				scopeFadeDraws != previousScopeFadeDraws.load(
									  std::memory_order_relaxed) ||
				reticleDraws != previousReticleDraws.load(
									std::memory_order_relaxed) ||
				housingDraws != previousHousingDraws.load(
									std::memory_order_relaxed) ||
				scopeFadeInstancedDraws +
						reticleInstancedDraws +
						housingInstancedDraws !=
					previousInstancedDraws.load(
						std::memory_order_relaxed) ||
				gunState != previousGunState.load(
								std::memory_order_relaxed) ||
				scopeFadeShapedDraws != previousShapedDraws.load(
										   std::memory_order_relaxed);
			if (firingSighted || changed) {
				logger::info(
					"Stage 4d.2d draw telemetry: gunState={} ({}), "
					"automaticDraws={} gated, observed DI:{} DII:{}, "
					"ScopeFade=DI:{} DII:{} @{} shaped:{}, "
					"Reticle=DI:{} DII:{} @{}, "
					"Housing=DI:{} DII:{} @{}",
					gunState,
					firingSighted ? "FireSighted" :
									(gunState ==
												static_cast<std::uint32_t>(
													RE::GUN_STATE::kSighted) ?
											"Sighted" :
											"other"),
					totalAutomaticDraws,
					observedDraws,
					observedInstancedDraws,
					scopeFadeDraws,
					scopeFadeInstancedDraws,
					scopeFadeOrdinal,
					scopeFadeShapedDraws,
					reticleDraws,
					reticleInstancedDraws,
					reticleOrdinal,
					housingDraws,
					housingInstancedDraws,
					housingOrdinal);
			}
			previousScopeFadeDraws.store(
				scopeFadeDraws,
				std::memory_order_relaxed);
			previousShapedDraws.store(
				scopeFadeShapedDraws,
				std::memory_order_relaxed);
			previousReticleDraws.store(
				reticleDraws,
				std::memory_order_relaxed);
			previousHousingDraws.store(
				housingDraws,
				std::memory_order_relaxed);
			previousInstancedDraws.store(
				scopeFadeInstancedDraws +
					reticleInstancedDraws +
					housingInstancedDraws,
				std::memory_order_relaxed);
			previousGunState.store(
				gunState,
				std::memory_order_relaxed);
		} else {
			// The telemetry above only runs while the readiness gate is open,
			// so the observed-draw counter has to be cleared here too or it
			// accumulates across every frame the gate was shut and reports a
			// meaningless total on the frame it reopens.
			automaticSTSObservedDrawsThisFrame.store(
				0U,
				std::memory_order_relaxed);
				automaticSTSObservedInstancedDrawsThisFrame.store(
				0U,
				std::memory_order_relaxed);
			automaticSTSScopeFadeShapedDrawsThisFrame.store(
				0U,
				std::memory_order_relaxed);
		}

#ifdef _DEBUG
		if (GetAsyncKeyState(VK_F4) & 1) {
			logger::info("Frame capture requested");
			rdoc_api->TriggerCapture();
		}
#endif  // _DEBUG

		// The TAA vtable hook and framework before-render callback both run
		// before the real Present. If neither produced the scope this frame,
		// composite now. The shared guard also prevents a double draw no
		// matter which Present hook is outermost.
		if (D3D::isEnableRender && !renderPassHandledThisFrame) {
			D3DInstance->Render();
			compositedThisFrame = lastRenderProducedComposite;
		}

		const auto result =
			oldFuncs.phookD3D11Present(pSwapChain, SyncInterval, Flags);
		// The next game frame must earn exact-geometry authority again. Do not
		// let a successful ScopeFade replacement suppress a later frame whose
		// draw is absent or arrives through DrawIndexedInstanced.
		automaticSTSExactScopeFadeReplacementThisFrame.store(
			false,
			std::memory_order_release);
		D3DInstance->ClearAutomaticSTSScopeFadeReplay();
		D3DInstance->ClearAutomaticSTSReticleReplay();
		D3DInstance->ClearAutomaticSTSReticleLayer();
		automaticSTSReplayFrameGeneration.fetch_add(
			1U,
			std::memory_order_acq_rel);
		renderedAtTAAThisFrame = false;
		compositedThisFrame = false;
		renderPassHandledThisFrame = false;
		return result;
	}

	HRESULT __stdcall D3D::ResizeBuffersHook(
		IDXGISwapChain* swapChain,
		UINT bufferCount,
		UINT width,
		UINT height,
		DXGI_FORMAT newFormat,
		UINT flags)
	{
		if (!oldFuncs.resizeBuffers) {
			return DXGI_ERROR_INVALID_CALL;
		}

		D3DInstance->ReleaseSizeDependentResources();

		const HRESULT result = oldFuncs.resizeBuffers(
			swapChain,
			bufferCount,
			width,
			height,
			newFormat,
			flags);
		if (SUCCEEDED(result)) {
			windowWidth = static_cast<int>(width);
			windowHeight = static_cast<int>(height);
			logger::info("Swap chain resized to {}x{}; scope resources will be recreated", width, height);
		} else {
			logger::error("ResizeBuffers failed with HRESULT 0x{:08X}", static_cast<std::uint32_t>(result));
		}
		return result;
	}

	D3D* D3D::GetSington()
	{
		static D3D instance;
		return &instance;
	}

	void D3D::InitRenderDoc()
	{
#ifdef _DEBUG
		HMODULE mod = LoadLibraryA("renderdoc.dll");
		if (!mod) {
			logger::error("Failed to load renderdoc.dll. Error code: {}", GetLastError());
			return;
		}

		pRENDERDOC_GetAPI RENDERDOC_GetAPI =
			(pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");
		if (!RENDERDOC_GetAPI) {
			logger::error("Failed to get RENDERDOC_GetAPI function. Error code: {}", GetLastError());
			return;
		}

		int ret = RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_6_0, (void**)&rdoc_api);
		if (ret != 1) {
			logger::error("RENDERDOC_GetAPI failed with return code: {}", ret);
			return;
		}
		int major, minor, patch;
		rdoc_api->GetAPIVersion(&major, &minor, &patch);
		logger::info("RenderDoc API v{}.{}.{} loaded", major, minor, patch);
		// Set RenderDoc options
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_AllowFullscreen, 1);
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_AllowVSync, 1);
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_APIValidation, 1);
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureAllCmdLists, 1);
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_CaptureCallstacks, 1);
		rdoc_api->SetCaptureOptionU32(eRENDERDOC_Option_RefAllResources, 1);

		logger::info("RenderDoc initialized successfully");
#endif
	}

	bool D3D::CreateAndEnableHook(void* target, void* hook, void** original, const char* hookName)
	{
		if (!target || !hook || !original || !IsExecutableAddress(target)) {
			logger::error("{} received an invalid hook target", hookName);
			return false;
		}
		if (MH_CreateHook(target, hook, original) != MH_OK) {
			logger::error("Failed to create {} hook", hookName);
			return false;
		}
		if (MH_EnableHook(target) != MH_OK) {
			logger::error("Failed to enable {} hook", hookName);
			return false;
		}
		logger::info(
			"Installed {} at {:p} ({})",
			hookName,
			target,
			DescribeCodeAddress(target));
		return true;
	}

	struct HookedRender_TAA
	{
		// thunk 函数
		static void thunk(
			RE::ImageSpaceEffectTemporalAA* This,
			RE::BSTriShape* a_geometry,
			RE::ImageSpaceEffectParam* a_param)
		{
			// 调用原函数
			const auto original = func.get();
			if (!original || !This) {
				logger::critical(
					"TAA callback received an invalid original function or instance");
				return;
			}

			D3DPERF_BeginEvent(0xffffffff, L"TAA");
			original(This, a_geometry, a_param);
			D3DPERF_EndEvent();
			isActive_TAA = This->IsActive();

			static std::once_flag loggedActiveTaa;
			static std::once_flag loggedInactiveTaa;
			auto& activityLog =
				isActive_TAA ? loggedActiveTaa : loggedInactiveTaa;
			std::call_once(activityLog, [&] {
				logger::info(
					"Stage 3b TAA callback observed: active={}, upscaler={}",
					isActive_TAA,
					upscalerMod != nullptr);
			});

			const auto& verification = MagnaScope::GetSettings();
			const bool renderEnabled =
				D3D::isEnableRender.load(std::memory_order_acquire);
			if (verification.AllowsComposite() && renderEnabled &&
				isActive_TAA && !renderPassHandledThisFrame) {
				// Preserve the original scope-rendering Stage 4 behavior behind the
				// composite gate. It is not reachable during Stage 3b.
				if (!upscalerMod) {
					renderedAtTAAThisFrame = true;
					D3DInstance->Render();
					compositedThisFrame = lastRenderProducedComposite;
					if (!compositedThisFrame && !renderPassHandledThisFrame) {
						renderedAtTAAThisFrame = false;
					}
				}
			} else if (verification.AllowsTAACapture() && renderEnabled &&
					   isActive_TAA) {
				if (!upscalerMod) {
					const bool captured =
						D3DInstance->CaptureVerificationTAASource();
					taaVerificationCapturedSinceFramework.store(
						captured,
						std::memory_order_release);
					if (!captured) {
						static std::once_flag loggedMissingTaaTarget;
						std::call_once(loggedMissingTaaTarget, [] {
							logger::warn(
								"Stage 3b TAA callback found no compatible bound "
								"color target; FrameworkPresent remains the fallback");
						});
					}
				}
			}
		}
		static inline REL::Relocation<decltype(thunk)*> func;
	};

	bool InstallGuardedTAAHook()
	{
		static std::mutex installLock;
		static bool attempted = false;
		static bool installed = false;
		std::scoped_lock lock{ installLock };
		if (attempted) {
			return installed;
		}
		attempted = true;

		if (!REX::FModule::IsRuntimeOG()) {
			logger::warn("TAA callback is not installed outside Fallout 4 OG");
			return false;
		}

		upscalerMod = DetectUpscalerOrFrameGeneration();
		REL::Relocation<std::uintptr_t> taaVtable{
			RE::ImageSpaceEffectTemporalAA::VTABLE[0]
		};
		const auto renderSlotAddress =
			taaVtable.address() + sizeof(std::uintptr_t);
		const auto originalRender =
			*reinterpret_cast<const std::uintptr_t*>(renderSlotAddress);
		if (!IsExecutableAddress(
				reinterpret_cast<const void*>(originalRender))) {
			logger::error(
				"TAA vtable slot 1 does not contain an executable target");
			return false;
		}

		// A vtable slot redirected into another DLL belongs to that plugin. Do
		// not overwrite it because that would silently break the other render
		// integration and could invalidate its call chain.
		HMODULE targetOwner = nullptr;
		const bool foundOwner = GetModuleHandleExW(
									GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
										GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
									reinterpret_cast<LPCWSTR>(originalRender),
									&targetOwner) != FALSE;
		const HMODULE falloutModule = GetModuleHandleW(nullptr);
		if (!foundOwner || targetOwner != falloutModule) {
			logger::warn(
				"TAA vtable slot 1 is already owned by another module; "
				"the callback was not replaced");
			return false;
		}

		// Publish the call-through before exposing the thunk in the vtable.
		// The render thread can execute this slot concurrently with
		// kGameDataReady, so the reverse order creates a short null-original
		// race even though installation is otherwise single-threaded.
		HookedRender_TAA::func =
			REL::Relocation<decltype(HookedRender_TAA::thunk)*>(
				originalRender);
		const auto replacedRender = taaVtable.write_vfunc(
			1,
			reinterpret_cast<std::uintptr_t>(&HookedRender_TAA::thunk));
		if (replacedRender != originalRender) {
			taaVtable.write_vfunc(1, replacedRender);
			logger::critical(
				"TAA vtable changed during installation; original slot restored");
			return false;
		}
		const auto installedRender =
			*reinterpret_cast<const std::uintptr_t*>(renderSlotAddress);
		if (installedRender !=
			reinterpret_cast<std::uintptr_t>(&HookedRender_TAA::thunk)) {
			taaVtable.write_vfunc(1, originalRender);
			logger::critical(
				"TAA vtable did not retain the callback; original slot restored");
			return false;
		}
		installed = true;
		logger::info(
			"Installed guarded OG TAA source-capture callback at {:p}; "
			"original={:p}",
			reinterpret_cast<void*>(renderSlotAddress),
			reinterpret_cast<void*>(originalRender));
		return true;
	}

	bool D3D::InstallVerificationTAAHook()
	{
		return InstallGuardedTAAHook();
	}

	DWORD __stdcall D3D::HookDX11_Init()
	{
		// Detect an already loaded upscaler without changing its module
		// reference count or forcing it to initialize out of order.
		upscalerMod = DetectUpscalerOrFrameGeneration();

		logger::info("HookDX11_Init");

		const auto* rendererData = RE::BSGraphics::GetRendererData();
		const auto* renderWindow = RE::BSGraphics::GetCurrentRendererWindow();
		if (!rendererData || !renderWindow || !renderWindow->swapChain ||
			!rendererData->device || !rendererData->context) {
			logger::error("Renderer objects are unavailable; DX11 hooks were not installed");
			return 1;
		}
		g_Swapchain = (IDXGISwapChain*)static_cast<void*>(renderWindow->swapChain);
		g_Device = (ID3D11Device*)static_cast<void*>(rendererData->device);
		g_Context = (ID3D11DeviceContext*)static_cast<void*>(rendererData->context);

		EnableDebugPrivilege();

		logger::info("Post D3D11CreateDeviceAndSwapChain");

		// 获取虚函数表
		pSwapChainVTable = *reinterpret_cast<DWORD_PTR**>(g_Swapchain.Get());
		pDeviceVTable = *reinterpret_cast<DWORD_PTR**>(g_Device.Get());
		pDeviceContextVTable = *reinterpret_cast<DWORD_PTR**>(g_Context.Get());

		sdh = ScopeData::ScopeDataHandler::GetSingleton();

		logger::info("Get VTable");

		if (MagnaScope::GetSettings().AllowsScopeFadeGeometry() &&
			!InitGeometryProbeEffect()) {
			InvalidateAutomaticSTSGeometry();
			logger::critical(
				"Stage 4d geometry probe initialization failed; "
				"all STS draws will pass through unchanged");
		}

		// MinHook may already be initialized by another DX11 integration.
		const auto minHookResult = MH_Initialize();
		if (minHookResult != MH_OK && minHookResult != MH_ERROR_ALREADY_INITIALIZED) {
			logger::error("Failed to initialize MinHook");
			return 1;
		}

		// 批量创建钩子
		const std::pair<DWORD_PTR*, HookInfo> hooks[] = {
			{ pSwapChainVTable, HookInfo{ 8, reinterpret_cast<void*>(PresentHook), reinterpret_cast<void**>(&oldFuncs.phookD3D11Present), "PresentHook" } },
			{ pSwapChainVTable, HookInfo{ 13, reinterpret_cast<void*>(ResizeBuffersHook), reinterpret_cast<void**>(&oldFuncs.resizeBuffers), "ResizeBuffersHook" } },
			{ pDeviceContextVTable, HookInfo{ 12, reinterpret_cast<void*>(DrawIndexedHook), reinterpret_cast<void**>(&oldFuncs.phookD3D11DrawIndexed), "DrawIndexedHook" } },
			// Windows SDK d3d11.h declares DrawIndexedInstanced at
			// ID3D11DeviceContext vtable slot 20. This diagnostic hook only
			// counts exact published STS identities and always forwards the
			// original draw unchanged.
			{ pDeviceContextVTable, HookInfo{ 20, reinterpret_cast<void*>(DrawIndexedInstancedHook), reinterpret_cast<void**>(&oldFuncs.phookD3D11DrawIndexedInstanced), "DrawIndexedInstancedHook" } },
		};

		// Which vtable we read matters as much as which slot. A wrapped device
		// context has its own vtable of forwarding thunks, and hooking those
		// only sees the draws that particular wrapper forwards.
		g_deviceContextVTable = pDeviceContextVTable;
		logger::info(
			"Device context vtable at {:p} ({}); slot 12 -> {:p}, slot 20 -> {:p}",
			static_cast<void*>(pDeviceContextVTable),
			DescribeCodeAddress(pDeviceContextVTable),
			reinterpret_cast<void*>(pDeviceContextVTable[12]),
			reinterpret_cast<void*>(pDeviceContextVTable[20]));

		// Record the address each hook actually took, not a re-read afterwards.
		// The slot can move while MinHook is working -- MH_CreateHook takes
		// over a hundred milliseconds -- and re-reading would store the value
		// it moved to as though we had hooked it, leaving the per-frame check
		// satisfied by a hook that is never called. The other implementation is
		// picked up by the first Present that sees it.
		for (const auto& [vtable, info] : hooks) {
			void* const target = reinterpret_cast<void*>(vtable[info.index]);
			if (!CreateAndEnableHook(
					target,
					info.hook,
					info.original,
					info.name)) {
				continue;
			}
			if (vtable != pDeviceContextVTable) {
				continue;
			}
			if (info.index == 12) {
				g_drawIndexedBinding.primaryTarget = target;
			} else if (info.index == 20) {
				g_drawIndexedInstancedBinding.primaryTarget = target;
			}
		}

		if (MagnaScope::GetSettings().AllowsTAACapture() &&
			!InstallGuardedTAAHook()) {
			logger::warn("TAA render hook guard failed; Present fallback remains active");
		}
		if (MagnaScope::GetSettings().AllowsTAACapture()) {
			logger::info("Render anchors active: TAA preferred, Present fallback");
		} else {
			logger::info(
				"ScopeFade source capture runs before first-person rendering; "
				"no TAA callback is required");
		}

		logger::info("Install Hook");

#ifdef _DEBUG
		if (rdoc_api && g_Device.Get()) {
			IDXGIDevice* pDXGIDevice = nullptr;
			if (SUCCEEDED(g_Device->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDXGIDevice))) {
				rdoc_api->SetActiveWindow((void*)pDXGIDevice, g_hWnd);
				logger::info("Set RenderDoc active window to {:x}", (uintptr_t)g_hWnd);
				pDXGIDevice->Release();
			} else {
				logger::error("Failed to get DXGI device for RenderDoc");
			}
		}
#endif  // _DEBUG

		return S_OK;
	}

	D3D11_HOOK_API void D3D::ImplHookDX11_Init(HMODULE hModule, void* hwnd)
	{
		g_hWnd = (HWND)hwnd;
		g_hModule = hModule;
		HookDX11_Init();
	}

	void D3D::QueryChangeReticleTexture() { bChangeAimTexture = true; }
	void D3D::ResetZoomDelta() { bResetZoomDelta = true; }
	void D3D::AdjustZoomDelta(float delta) { gameZoomDelta += delta; }
	void D3D::SetZoom(float zoom) { gameZoomDelta = zoom; }
	void D3D::SetFinishAimAnim(bool flag) { bFinishAimAnim = flag; }
	void D3D::SetScopeEffect(bool flag) { isEnableScopeEffect = flag; }
	bool D3D::GetScopeEffect() { return isEnableScopeEffect; }
	void D3D::SetInterfaceTextRefresh(bool flag)
	{
		bRefreshChar.store(flag, std::memory_order_release);
		bEnableEditMode.store(false, std::memory_order_release);
	}

	void D3D::SetGameConstData(GameConstBuffer c) { gameConstBuffer = c; }
	void D3D::InitPlayerData(RE::PlayerCharacter* pl, RE::PlayerCamera* pc)
	{
		player = pl;
		pcam = pc;
	}
	void D3D::SetNVG(int flag) { bEnableNVG = flag; }
	void D3D::StartScope(bool flag) { bStartScope = flag; }

	D3D::OldFuncs D3D::oldFuncs;
	std::atomic_bool D3D::isEnableRender = false;
	bool D3D::frameworkRenderAnchor = false;
	std::atomic<float> D3D::projectedLensX = 0.0F;
	std::atomic<float> D3D::projectedLensY = 0.0F;
	std::atomic<float> D3D::projectedAimX = 0.0F;
	std::atomic<float> D3D::projectedAimY = 0.0F;
	std::atomic<float> D3D::projectedLensRadiusX = 0.0F;
	std::atomic<float> D3D::projectedLensRadiusY = 0.0F;
	std::atomic<float> D3D::projectedActivationProgress = 0.0F;
	std::atomic<float> D3D::projectedSourceWidth = 0.0F;
	std::atomic<float> D3D::projectedSourceHeight = 0.0F;
	std::atomic_bool D3D::projectedAutomaticSTS = false;
	std::atomic<float> D3D::projectedEyeOffsetX = 0.0F;
	std::atomic<float> D3D::projectedEyeOffsetY = 0.0F;
	std::atomic<float> D3D::projectedEyeReliefDelta = 0.0F;
	std::atomic<float> D3D::projectedLensBasisXX = 0.0F;
	std::atomic<float> D3D::projectedLensBasisXY = 0.0F;
	std::atomic<float> D3D::projectedLensBasisZX = 0.0F;
	std::atomic<float> D3D::projectedLensBasisZY = 0.0F;
	std::atomic<float> D3D::projectedPhysicalEyeBoxBlend = 0.0F;
	std::atomic_bool D3D::projectedPhysicalEyeBoxReady = false;
	std::atomic_bool D3D::projectedTrackingReady = false;
	std::atomic_uint64_t D3D::projectedLensSequence = 0U;
	std::atomic<std::uintptr_t> D3D::automaticSTSVertexBuffer = 0;
	std::atomic<std::uintptr_t> D3D::automaticSTSIndexBuffer = 0;
	std::atomic_uint32_t D3D::automaticSTSIndexCount = 0;
	std::atomic_uint32_t D3D::automaticSTSVertexStride = 0;
	std::atomic_uint32_t D3D::automaticSTSVertexDataOffset = 0;
	std::atomic_uint32_t D3D::automaticSTSIndexDataOffset = 0;
	std::atomic_bool D3D::automaticSTSGeometryReady = false;
	std::atomic<std::uintptr_t> D3D::automaticSTSReticleVertexBuffer = 0;
	std::atomic<std::uintptr_t> D3D::automaticSTSReticleIndexBuffer = 0;
	std::atomic_uint32_t D3D::automaticSTSReticleIndexCount = 0;
	std::atomic_uint32_t D3D::automaticSTSReticleVertexCount = 0;
	std::atomic_uint32_t D3D::automaticSTSReticleVertexStride = 0;
	std::atomic_uint32_t D3D::automaticSTSReticleVertexDataOffset = 0;
	std::atomic_uint32_t D3D::automaticSTSReticleIndexDataOffset = 0;
	std::atomic_uint64_t D3D::automaticSTSReticleVertexDescriptor = 0;
	std::atomic_uint64_t D3D::automaticSTSReticleGeometryGeneration = 0;
	std::atomic_bool D3D::automaticSTSReticleGeometryReady = false;
	std::array<
		D3D::AutomaticSTSReticleGeometryIdentity,
		D3D::kMaxAutomaticSTSReticleGeometries>
		D3D::automaticSTSReticleGeometries{};
	std::atomic_uint32_t D3D::automaticSTSReticleGeometryCount = 0U;
	std::atomic_uint64_t D3D::automaticSTSReticleSetSequence = 0U;
	std::atomic<std::uintptr_t> D3D::automaticSTSHousingVertexBuffer = 0;
	std::atomic<std::uintptr_t> D3D::automaticSTSHousingIndexBuffer = 0;
	std::atomic_uint32_t D3D::automaticSTSHousingIndexCount = 0;
	std::atomic_uint32_t D3D::automaticSTSHousingVertexStride = 0;
	std::atomic_uint32_t D3D::automaticSTSHousingVertexDataOffset = 0;
	std::atomic_uint32_t D3D::automaticSTSHousingIndexDataOffset = 0;
	std::atomic_bool D3D::automaticSTSHousingGeometryReady = false;
	std::atomic_uint32_t D3D::automaticSTSScopeFadeDrawsThisFrame = 0;
	std::atomic_bool D3D::automaticSTSScopeFadeVisibleLastFrame = false;
	std::atomic_uint32_t D3D::automaticSTSReticleDrawsThisFrame = 0;
	std::atomic_uint32_t D3D::automaticSTSHousingDrawsThisFrame = 0;
	std::atomic_uint32_t
		D3D::automaticSTSScopeFadeInstancedDrawsThisFrame = 0;
	std::atomic_uint32_t
		D3D::automaticSTSReticleInstancedDrawsThisFrame = 0;
	std::atomic_uint32_t
		D3D::automaticSTSHousingInstancedDrawsThisFrame = 0;
	std::atomic_bool
		D3D::automaticSTSExactScopeFadeReplacementThisFrame = false;
	std::atomic_uint32_t D3D::automaticSTSDrawOrdinalThisFrame = 0;
	std::atomic_uint32_t D3D::automaticSTSLastScopeFadeOrdinal = 0;
	std::atomic_uint32_t D3D::automaticSTSLastReticleOrdinal = 0;
	std::atomic_uint32_t D3D::automaticSTSLastHousingOrdinal = 0;
	std::atomic_uint32_t D3D::automaticSTSGunState = 0;
	bool D3D::bStartScope = false;
	bool D3D::bFinishAimAnim = false;
	std::atomic_bool D3D::bRefreshChar{ true };
	int D3D::bEnableNVG = 0;
	bool D3D::bQueryRender = false;
	bool D3D::bIsInGame = false;
	bool D3D::isEnableScopeEffect = false;
	std::atomic_bool D3D::bEnableEditMode{ false };
	float D3D::editZoomMin = 1.0F;
	float D3D::editZoomMax = 4.0F;
	std::atomic<float> D3D::scopeFadeMagnification{ 2.0F };
	std::atomic<float> D3D::scopeImageDenoise{ 0.0F };
	std::atomic<float> D3D::scopeImageSharpen{ 0.0F };
	std::atomic<float> D3D::scopeFishEyeStrength{ 0.0F };
	std::atomic<float> D3D::scopeFishEyePower{ 2.0F };
	std::atomic<float> D3D::scopeEdgeRefractionStrength{ 0.0F };
	std::atomic<float> D3D::scopeEdgeRefractionWidth{ 0.15F };
	std::atomic<float> D3D::scopeEdgeChromaticAberration{ 0.0F };
	std::atomic<float> D3D::scopeReticleMagnification{ 1.0F };
	std::atomic<float> D3D::scopeReticleSize{ 4.0F };
	std::atomic<float> D3D::scopeReticleOffsetX{ 0.0F };
	std::atomic<float> D3D::scopeReticleOffsetY{ 0.0F };
	std::atomic<float> D3D::scopeReticleShadowStrength{ 0.0F };
	std::atomic<float> D3D::scopeReticleParallaxStrength{ 1.0F };
	std::atomic<float> D3D::scopeEyeBoxRadius{ 2.0F };
	std::atomic<float> D3D::scopeVignetteReach{ 9.0F };
	std::atomic<float> D3D::scopeVignetteSharpness{ 3.0F };
	std::atomic<float> D3D::scopeEyeBoxMaxTravel{ 4.0F };
	std::atomic<float> D3D::scopeSceneParallaxStrength{ 0.0F };
	std::atomic<float> D3D::scopeOpticalLagStrength{ 1.0F };
	std::atomic<float> D3D::scopeSceneDepth{ 1.0F };
	std::atomic<float> D3D::scopeShadowDepth{ 1.0F };
	std::atomic<float> D3D::scopeImageStillness{ 0.0F };
	std::atomic<float> D3D::scopeAxialBreathing{ 0.0F };
	std::atomic<float> D3D::scopeRecenterSpeed{ 1.0F };
	std::atomic<float> D3D::scopeApertureScaleRatio{ 1.0F };
	std::atomic<float> D3D::scopeTubeDepth{ 0.0F };
	std::atomic_uint32_t D3D::automaticSTSScopeFadeShapedDrawsThisFrame = 0U;
	std::atomic_uint32_t D3D::automaticSTSObservedDrawsThisFrame = 0U;
	std::atomic_uint32_t D3D::automaticSTSObservedInstancedDrawsThisFrame = 0U;
	std::atomic<float> D3D::scopeLensOffsetX{ 0.0F };
	std::atomic<float> D3D::scopeLensOffsetY{ 0.0F };
	std::atomic<float> D3D::scopeLensScale{ 1.0F };
	std::atomic<float> D3D::scopeBreathPhase{ 0.0F };
	std::atomic<float> D3D::scopeBreathRate{ 0.25F };
	std::atomic<float> D3D::scopeBreathSway{ 0.0F };
	std::atomic<float> D3D::scopeBreathDrift{ 0.0F };
	std::atomic<float> D3D::scopeBreathFigure{ 0.25F };
	std::atomic<float> D3D::scopeBreathHold{ 0.0F };
	std::atomic<float> D3D::scopeBreathPupilFollow{ 1.0F };
	bool D3D::bLegacyMode;

	std::once_flag D3D::flagOnce;
}
