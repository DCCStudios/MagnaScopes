#define MAGNASCOPE_INTERNAL
#include "MagnaScopeAPI.h"

#include "ScopeProfile.h"
#include "ScopeResolver.h"

#include <mutex>
#include <string>
#include <vector>

// Provided by main.cpp. The API is a thin, well-behaved front for state the
// plugin already owns; it deliberately introduces no new ownership.
extern ScopeData::ScopeProfile* currentData;
extern RE::PlayerCharacter* player;

namespace MagnaScopeAPI
{
	namespace
	{
		// Queued work, drained on the game thread. Callers may be on any
		// thread; nothing here touches an engine object at queue time.
		struct Command
		{
			enum class Kind : std::uint8_t
			{
				kApertureOverride,
				kReticleOverride,
				kClearOverrides,
				kSightSwap,
			};

			Kind kind{ Kind::kClearOverrides };
			std::string name;
			std::int32_t index{ -1 };
			// Who asked, so a conflict between two consumers is visible in the
			// log rather than a silent last-writer-wins.
			std::string owner;
		};

		std::mutex commandMutex;
		std::vector<Command> pendingCommands;

		// Published snapshots. Every getter is callable from any thread by an
		// external plugin, so none of them may walk the engine's node tree or
		// touch the session map directly -- both are game-thread-owned. The
		// game thread fills these once per tick and getters copy out of them.
		std::mutex snapshotMutex;
		ScopeInfoV1 scopeSnapshot{};
		bool scopeSnapshotValid = false;
		std::vector<NodeInfoV1> nodeSnapshot;

		std::mutex overrideMutex;
		std::string apertureOverride;
		std::string reticleOverride;
		std::string apertureOverrideOwner;
		std::string reticleOverrideOwner;

		void Enqueue(Command command)
		{
			std::scoped_lock lock(commandMutex);
			// A runaway caller must not grow this without bound; the queue is
			// drained every game-thread tick, so anything past a few entries in
			// one tick is a caller bug rather than load.
			if (pendingCommands.size() < 256U) {
				pendingCommands.push_back(std::move(command));
			}
		}

		// Resolves a shape pointer to its node name immediately, so nothing but
		// a string ever crosses into MagnaScope's own state.
		[[nodiscard]] std::string NameOfShape(void* bsTriShape)
		{
			if (!bsTriShape) {
				return {};
			}
			auto* object = static_cast<RE::NiAVObject*>(bsTriShape);
			return std::string(object->name.c_str() ? object->name.c_str() : "");
		}

		bool SetApertureOverrideImpl(void* shape)
		{
			Enqueue(Command{
				Command::Kind::kApertureOverride, NameOfShape(shape), -1, {} });
			return true;
		}

		bool SetApertureOverrideByNameImpl(const char* nodeName)
		{
			Enqueue(Command{
				Command::Kind::kApertureOverride,
				nodeName ? nodeName : "",
				-1,
				{} });
			return true;
		}

		bool SetReticleOverrideImpl(void* shape)
		{
			Enqueue(Command{
				Command::Kind::kReticleOverride, NameOfShape(shape), -1, {} });
			return true;
		}

		bool SetReticleOverrideByNameImpl(const char* nodeName)
		{
			Enqueue(Command{
				Command::Kind::kReticleOverride,
				nodeName ? nodeName : "",
				-1,
				{} });
			return true;
		}

		bool ClearOverridesImpl()
		{
			Enqueue(Command{ Command::Kind::kClearOverrides, {}, -1, {} });
			return true;
		}

		bool TriggerSightSwapImpl(std::int32_t sightIndex)
		{
			Enqueue(Command{ Command::Kind::kSightSwap, {}, sightIndex, {} });
			return true;
		}

		void CopyString(char* destination, std::size_t size, const std::string& source)
		{
			if (!destination || size == 0U) {
				return;
			}
			const auto copied = std::min(source.size(), size - 1U);
			std::memcpy(destination, source.data(), copied);
			destination[copied] = '\0';
		}

		bool GetEquippedScopeImpl(ScopeInfoV1* out)
		{
			if (!out) {
				return false;
			}
			std::scoped_lock lock(snapshotMutex);
			if (!scopeSnapshotValid) {
				return false;
			}
			*out = scopeSnapshot;
			return true;
		}

		// Game thread only.
		void FillScopeSnapshot(ScopeInfoV1* out)
		{
			*out = ScopeInfoV1{};
			CopyString(
				out->weaponPlugin, sizeof(out->weaponPlugin),
				currentData->sourcePlugin);
			out->weaponFormID = currentData->sourceFormID;
			CopyString(out->omodKey, sizeof(out->omodKey), currentData->omodKey);
			CopyString(
				out->apertureSurface, sizeof(out->apertureSurface),
				currentData->shaderData.apertureSurface);
			CopyString(
				out->reticleSurface, sizeof(out->reticleSurface),
				currentData->shaderData.reticleSurface);

			const auto& state = MagnaScope::SessionStateFor(*currentData);
			out->magnification = currentData->shaderData.minZoom;
			out->secondarySightIndex = state.secondaryIndex;
			out->secondarySightCount =
				static_cast<std::int32_t>(currentData->secondarySights.size());
			out->reticleIndex = state.reticleIndex;
			out->reticleCount = -1;  // Filled by the caller-facing wrapper below.
			out->variantIndex =
				MagnaScope::ActiveVariantIndex(*currentData, state);
			out->variantCount =
				static_cast<std::int32_t>(currentData->variants.variants.size());
		}

		void FlattenNode(
			RE::NiAVObject* node,
			std::int32_t parentIndex,
			std::vector<NodeInfoV1>& out)
		{
			if (!node) {
				return;
			}

			NodeInfoV1 info{};
			CopyString(
				info.name,
				sizeof(info.name),
				node->name.c_str() ? node->name.c_str() : "");
			info.parentIndex = parentIndex;
			info.localTranslation[0] = node->local.translate.x;
			info.localTranslation[1] = node->local.translate.y;
			info.localTranslation[2] = node->local.translate.z;
			for (int row = 0; row < 3; ++row) {
				for (int column = 0; column < 3; ++column) {
					info.localRotation[row * 3 + column] =
						node->local.rotate.entry[row][column];
				}
			}
			info.localScale = node->local.scale;
			info.culled = node->GetAppCulled() ? 1U : 0U;
			info.isShape = node->IsTriShape() ? 1U : 0U;

			const auto selfIndex = static_cast<std::int32_t>(out.size());
			out.push_back(info);

			if (auto* asNode = node->IsNode()) {
				for (const auto& child : asNode->children) {
					FlattenNode(child.get(), selfIndex, out);
				}
			}
		}

		std::uint32_t SnapshotNodeTreeImpl(NodeInfoV1* out, std::uint32_t maxNodes)
		{
			std::scoped_lock lock(snapshotMutex);
			if (!out || maxNodes == 0U) {
				return static_cast<std::uint32_t>(nodeSnapshot.size());
			}
			const auto copied =
				std::min<std::size_t>(nodeSnapshot.size(), maxNodes);
			std::memcpy(out, nodeSnapshot.data(), copied * sizeof(NodeInfoV1));
			return static_cast<std::uint32_t>(copied);
		}

		InterfaceV1 theInterface{
			1U,
			SetApertureOverrideImpl,
			SetApertureOverrideByNameImpl,
			SetReticleOverrideImpl,
			SetReticleOverrideByNameImpl,
			ClearOverridesImpl,
			TriggerSightSwapImpl,
			GetEquippedScopeImpl,
			SnapshotNodeTreeImpl,
		};
	}

	InterfaceV1* GetInterface()
	{
		return &theInterface;
	}

	std::string GetApertureOverride()
	{
		std::scoped_lock lock(overrideMutex);
		return apertureOverride;
	}

	std::string GetReticleOverride()
	{
		std::scoped_lock lock(overrideMutex);
		return reticleOverride;
	}

	void RefreshSnapshots(int reticleCount)
	{
		// Game thread only. Built here so every getter is a lock-and-copy from
		// plain data, never a walk of live engine state on a caller's thread.
		std::vector<NodeInfoV1> nodes;
		if (player) {
			if (auto* root = player->firstPerson3D.get()) {
				nodes.reserve(256U);
				FlattenNode(root, -1, nodes);
			}
		}

		ScopeInfoV1 scope{};
		const bool haveScope = currentData != nullptr;
		if (haveScope) {
			FillScopeSnapshot(&scope);
			scope.reticleCount = reticleCount;
		}

		std::scoped_lock lock(snapshotMutex);
		nodeSnapshot = std::move(nodes);
		scopeSnapshot = scope;
		scopeSnapshotValid = haveScope;
	}

	void DrainCommands()
	{
		std::vector<Command> commands;
		{
			std::scoped_lock lock(commandMutex);
			commands.swap(pendingCommands);
		}

		for (const auto& command : commands) {
			switch (command.kind) {
			case Command::Kind::kApertureOverride:
			{
				std::scoped_lock lock(overrideMutex);
				if (!apertureOverride.empty() &&
					apertureOverride != command.name) {
					logger::info(
						"API aperture override replaced: '{}' -> '{}'",
						apertureOverride,
						command.name);
				}
				apertureOverride = command.name;
				break;
			}
			case Command::Kind::kReticleOverride:
			{
				std::scoped_lock lock(overrideMutex);
				if (!reticleOverride.empty() &&
					reticleOverride != command.name) {
					logger::info(
						"API reticle override replaced: '{}' -> '{}'",
						reticleOverride,
						command.name);
				}
				reticleOverride = command.name;
				break;
			}
			case Command::Kind::kClearOverrides:
			{
				std::scoped_lock lock(overrideMutex);
				apertureOverride.clear();
				reticleOverride.clear();
				break;
			}
			case Command::Kind::kSightSwap:
			{
				if (!currentData) {
					break;
				}
				auto& state = MagnaScope::SessionStateFor(*currentData);
				const auto count =
					static_cast<std::int32_t>(currentData->secondarySights.size());
				state.secondaryIndex =
					command.index >= 0 && command.index < count ?
						command.index :
						-1;
				if (state.secondaryIndex >= 0) {
					// Keeps the blend-back symmetric, same as the hotkey path.
					state.lastSightIndex = state.secondaryIndex;
				}
				state.userTouched = true;
				break;
			}
			}
		}
	}
}
