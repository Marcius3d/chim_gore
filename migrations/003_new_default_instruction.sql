-- CHIM Gore 0.3: replace the old default instruction (only if the user never changed it).
UPDATE plugins.chim_gore_settings
SET value = '', updated_at = CURRENT_TIMESTAMP
WHERE key = 'instruction'
  AND value LIKE '{NPC} briefly reacts to the most gruesome moment of the fight that just ended, in their own voice and personality:%';
