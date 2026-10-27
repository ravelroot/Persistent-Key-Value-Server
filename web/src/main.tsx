import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import '@fontsource-variable/archivo/wdth.css'
import '@fontsource/ibm-plex-mono/400.css'
import '@fontsource/ibm-plex-mono/500.css'
import './styles.css'
import './features.css'
import { App } from './App'
import { ToastProvider, applyTheme, initialTheme } from './state'

applyTheme(initialTheme())

const root = document.getElementById('root')
if (root) {
  createRoot(root).render(
    <StrictMode>
      <ToastProvider>
        <App />
      </ToastProvider>
    </StrictMode>,
  )
}
