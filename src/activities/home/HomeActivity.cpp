#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <Xtc.h>

#include <cstring>
#include <vector>

#include "Battery.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ScreenComponents.h"
#include "fontIds.h"
#include "util/StringUtils.h"

void HomeActivity::taskTrampoline(void* param) {
  auto* self = static_cast<HomeActivity*>(param);
  self->displayTaskLoop();
}

int HomeActivity::getMenuItemCount() const {
  int count = 3;  // Browse files, File transfer, Settings
  if (hasContinueReading) count++;
  if (hasOpdsUrl) count++;
  return count;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // Check if we have a book to continue reading
  hasContinueReading = !APP_STATE.openEpubPath.empty() && SdMan.exists(APP_STATE.openEpubPath.c_str());

  // Check if OPDS browser URL is configured
  hasOpdsUrl = strlen(SETTINGS.opdsServerUrl) > 0;

  if (hasContinueReading) {
    // Extract filename from path for display
    lastBookTitle = APP_STATE.openEpubPath;
    const size_t lastSlash = lastBookTitle.find_last_of('/');
    if (lastSlash != std::string::npos) {
      lastBookTitle = lastBookTitle.substr(lastSlash + 1);
    }

    // If epub, try to load the metadata for title/author and cover
    if (StringUtils::checkFileExtension(lastBookTitle, ".epub")) {
      Epub epub(APP_STATE.openEpubPath, "/.crosspoint");
      epub.load(false);
      epub.loadProgressFile();
      continueReadingProgress = epub.getCurrentProgress();
      bookSize = epub.getBookSize();
      currentBookPosition = bookSize * continueReadingProgress / 100;
      if (!epub.getTitle().empty()) {
        lastBookTitle = std::string(epub.getTitle());
      }
      if (!epub.getAuthor().empty()) {
        lastBookAuthor = std::string(epub.getAuthor());
      }
      // Try to generate thumbnail image for Continue Reading card
      if (epub.generateThumbBmp()) {
        coverBmpPath = epub.getThumbBmpPath();
        hasCoverImage = true;
      }
    } else if (StringUtils::checkFileExtension(lastBookTitle, ".xtch") ||
               StringUtils::checkFileExtension(lastBookTitle, ".xtc")) {
      // Handle XTC file
      Xtc xtc(APP_STATE.openEpubPath, "/.crosspoint");
      if (xtc.load()) {
        if (!xtc.getTitle().empty()) {
          lastBookTitle = std::string(xtc.getTitle());
        }
        // Try to generate thumbnail image for Continue Reading card
        if (xtc.generateThumbBmp()) {
          coverBmpPath = xtc.getThumbBmpPath();
          hasCoverImage = true;
        }
      }
      // Remove extension from title if we don't have metadata
      if (StringUtils::checkFileExtension(lastBookTitle, ".xtch")) {
        lastBookTitle.resize(lastBookTitle.length() - 5);
      } else if (StringUtils::checkFileExtension(lastBookTitle, ".xtc")) {
        lastBookTitle.resize(lastBookTitle.length() - 4);
      }
    }
  }

  selectorIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&HomeActivity::taskTrampoline, "HomeActivityTask",
              4096,               // Stack size (increased for cover image rendering)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void HomeActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  // Free any existing buffer first
  freeCoverBuffer();

  const size_t bufferSize = GfxRenderer::getBufferSize();
  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!coverBuffer) {
    return false;
  }

  memcpy(coverBuffer, frameBuffer, bufferSize);
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer) {
    return false;
  }

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  const size_t bufferSize = GfxRenderer::getBufferSize();
  memcpy(frameBuffer, coverBuffer, bufferSize);
  return true;
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const bool prevPressed = mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Left);
  const bool nextPressed = mappedInput.wasPressed(MappedInputManager::Button::Down) ||
                           mappedInput.wasPressed(MappedInputManager::Button::Right);

  const int menuCount = getMenuItemCount();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Calculate dynamic indices based on which options are available
    int idx = 0;
    const int continueIdx = hasContinueReading ? idx++ : -1;
    const int browseFilesIdx = idx++;
    const int opdsLibraryIdx = hasOpdsUrl ? idx++ : -1;
    const int fileTransferIdx = idx++;
    const int settingsIdx = idx;

    if (selectorIndex == continueIdx) {
      onContinueReading();
    } else if (selectorIndex == browseFilesIdx) {
      onReaderOpen();
    } else if (selectorIndex == opdsLibraryIdx) {
      onOpdsBrowserOpen();
    } else if (selectorIndex == fileTransferIdx) {
      onFileTransferOpen();
    } else if (selectorIndex == settingsIdx) {
      onSettingsOpen();
    }
  } else if (prevPressed) {
    selectorIndex = (selectorIndex + menuCount - 1) % menuCount;
    updateRequired = true;
  } else if (nextPressed) {
    selectorIndex = (selectorIndex + 1) % menuCount;
    updateRequired = true;
  }
}

void HomeActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void HomeActivity::render() {
  // If we have a stored cover buffer, restore it instead of clearing
  const bool bufferRestored = coverBufferStored && restoreCoverBuffer();
  if (!bufferRestored) {
    renderer.clearScreen();
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  constexpr int cornerRadius = 5;
  constexpr int margin = 50;
  constexpr int sideMargin = 20;
  constexpr int bottomMargin = 60;

  const int bookCardX = sideMargin;
  const int bookCardWidth = pageWidth - 2 * sideMargin;
  const int bookCardY = margin;
  const int bookCardHeight = pageHeight / 3;
  const int bookCardCornerRadius = 5;
  const int bookCardInnerMargin = 25;

  // --- Top "book" card for the current title (selectorIndex == 0) ---
  const int bookWidth = pageWidth / 3;
  const int bookHeight = bookCardHeight - 2 * bookCardInnerMargin;
  const int bookX = (pageWidth - bookWidth) / 2;
  constexpr int bookY = 30;
  const bool bookSelected = hasContinueReading && selectorIndex == 0;

  // Draw book card (rectangle with rounded corners)
  renderer.drawRoundedRect(bookCardX, bookCardY, bookCardWidth, bookCardHeight, bookCardCornerRadius);

  // Draw book cover
  if (hasContinueReading && hasCoverImage && !coverBmpPath.empty()) {

    // First time: load cover from SD and render
    if (!coverRendered) {
      FsFile file;
      if (SdMan.openFileForRead("HOME", coverBmpPath, file)) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          // Calculate position to center image within the book card
          int coverX, coverY;

          coverX = bookCardX + bookCardInnerMargin;
          coverY = bookCardY + bookCardInnerMargin;

          // Draw the cover image centered within the book card
          renderer.drawBitmap(bitmap, coverX, coverY, bookWidth, bookHeight);

          // Store the buffer with cover image for fast navigation
          coverBufferStored = storeCoverBuffer();
          coverRendered = true;
        }
        file.close();
      }
    }

    // Draw book title and author
    const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int textX = bookCardX + bookCardInnerMargin + bookWidth;
    const int textY = bookCardY + bookCardInnerMargin;

    bool truncated = false;
    int textWidth = renderer.getTextWidth(UI_12_FONT_ID, lastBookTitle.c_str());
    const int ellipsisWidth = renderer.getTextWidth(UI_12_FONT_ID, "...");

    while (textWidth > bookCardWidth - bookWidth - 2 * bookCardInnerMargin - ellipsisWidth) {
      StringUtils::utf8RemoveLastChar(lastBookTitle);
      textWidth = renderer.getTextWidth(UI_12_FONT_ID, lastBookTitle.c_str());
      truncated = true;
    }

    if (truncated) {
      lastBookTitle += "...";
    }

    renderer.drawText(UI_12_FONT_ID, textX, textY, lastBookTitle.c_str());
    renderer.drawText(UI_10_FONT_ID, textX, textY + lineHeight + 5, lastBookAuthor.c_str());

    // Draw progress bar below text
    const int progressBarHeight = 6;
    const int progressBarX = bookCardX + bookCardInnerMargin + bookWidth;
    const int progressBarEndX = bookCardX + bookCardWidth - bookCardInnerMargin;
    const int progressBarY = bookCardY + bookCardHeight - bookCardInnerMargin - progressBarHeight;
    const int progressBarWidth = progressBarEndX - progressBarX;
    const int filledWidth = (progressBarWidth * continueReadingProgress) / 100;
    renderer.drawRect(progressBarX, progressBarY, progressBarWidth, progressBarHeight - 2);
    renderer.fillRect(progressBarX - 1, progressBarY - 1, filledWidth, progressBarHeight);

    // Draw progress percentage at bottom right of card
    const std::string progressText = std::to_string(continueReadingProgress) + "%";
    const int progressTextWidth = renderer.getTextWidth(UI_10_FONT_ID, progressText.c_str());
    const int progressTextX = bookCardX + bookCardWidth - bookCardInnerMargin - progressTextWidth;
    const int progressTextY = bookCardY + bookCardHeight - bookCardInnerMargin - renderer.getLineHeight(UI_10_FONT_ID) - progressBarHeight - 4;
    renderer.drawText(UI_10_FONT_ID, progressTextX, progressTextY, progressText.c_str());

    // Draw book position text at bottom left of card
    const std::string bookPositionText = std::to_string(currentBookPosition) + " / " + std::to_string(bookSize);
    const int bookPositionTextX = bookCardX + bookCardInnerMargin + bookWidth;
    const int bookPositionTextY = progressTextY;
    renderer.drawText(UI_10_FONT_ID, bookPositionTextX, bookPositionTextY, bookPositionText.c_str());
  }

  // --- Bottom menu tiles ---
  // Build menu items dynamically
  std::vector<const char*> menuItems = {"Continue Reading", "Browse Files", "File Transfer", "Settings"};
  if (hasOpdsUrl) {
    // Insert Calibre Library after Browse Files
    menuItems.insert(menuItems.begin() + 2, "Calibre Library");
  }

  const int menuTileWidth = pageWidth - 2 * sideMargin;
  constexpr int menuTileHeight = 55;
  constexpr int menuSpacing = 8;
  const int totalMenuHeight =
      static_cast<int>(menuItems.size()) * menuTileHeight + (static_cast<int>(menuItems.size()) - 1) * menuSpacing;

  int menuStartY = bookCardY + bookCardHeight + 15;
  // Ensure we don't collide with the bottom button legend
  const int maxMenuStartY = pageHeight - bottomMargin - totalMenuHeight - margin;
  if (menuStartY > maxMenuStartY) {
    menuStartY = maxMenuStartY;
  }

  for (size_t i = 0; i < menuItems.size(); ++i) {
    const int overallIndex = static_cast<int>(i); // + (hasContinueReading ? 1 : 0);
    constexpr int tileX = sideMargin;
    const int tileY = menuStartY + static_cast<int>(i) * (menuTileHeight + menuSpacing);
    const bool selected = selectorIndex == overallIndex;

    if (selected) {
      renderer.fillRoundedRect(tileX, tileY, menuTileWidth, menuTileHeight, cornerRadius);
    } else {
      renderer.drawRoundedRect(tileX, tileY, menuTileWidth, menuTileHeight, cornerRadius);
    }

    const char* label = menuItems[i];
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, label);
    const int textX = tileX + (menuTileWidth - textWidth) / 2;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textY = tileY + (menuTileHeight - lineHeight) / 2;  // vertically centered assuming y is top of text

    // Invert text when the tile is selected, to contrast with the filled background
    renderer.drawText(UI_10_FONT_ID, textX, textY, label, !selected);
  }

  const auto labels = mappedInput.mapLabels("", "Confirm", "Up", "Down");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // get percentage so we can align text properly
  const uint16_t percentage = battery.readPercentage();
  const auto percentageText = showBatteryPercentage ? std::to_string(percentage) + "%" : "";
  const auto batteryX = pageWidth - sideMargin * 2 - renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
  ScreenComponents::drawBattery(renderer, batteryX, 10, showBatteryPercentage);

  renderer.displayBuffer();
}

std::string HomeActivity::getContinueReadingText() const {

  if (continueReadingProgress <= 0) {
    return "Continue Reading";
  }

  return "Continue Reading " + std::to_string(continueReadingProgress) + "%";
}