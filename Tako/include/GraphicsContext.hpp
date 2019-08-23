#pragma once
#include "Window.hpp"
#include <memory>
#include "Event.hpp"

namespace tako
{
	class GraphicsContext : public IEventHandler
	{
	public:
		GraphicsContext(WindowHandle handle, int width, int height);
		~GraphicsContext();
		void Present();
		void Resize(int width, int height);
		virtual void HandleEvent(Event& evt) override;
	private:
		class ContextImpl;
		std::unique_ptr<ContextImpl> m_impl;
	};
}
